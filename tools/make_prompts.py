"""Button prompt icons in the game's style: bronze riveted plates like its own legend icons.

Rebuilds runtime/assets/prompts/<style>/*.png from Kenney's "Input Prompts" pack (CC0, kenney.nl):
    python tools/make_prompts.py "<pack folder>"   (the folder with "PlayStation Series", ...)

Every icon keeps Kenney's outline. Face buttons sit on an octagon plate with rivets, the symbol in the
PlayStation colours or white; other buttons are bronze in their own shape with white letters. Sticks
(and the labelled buttons) stay bronze with only the direction arrows and the letter white, their ring a
light bronze line shaded in black.
"""
import math
import os
import random
import re
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter

N = 128
STYLES = {"playstation": "PlayStation Series", "xbox": "Xbox Series", "steamdeck": "Steam Deck",
          "keyboard": "Keyboard & Mouse"}
SYMBOL = {"cross": (40, 165, 235), "triangle": (55, 190, 60), "circle": (225, 60, 60), "square": (225, 110, 200)}
WHITE = (255, 255, 255)
RING = (215, 165, 110)
PLATE_PIECES = ("stick", "options", "share", "create")


def binary(m):
    return m.point(lambda v: 255 if v > 128 else 0)


def fill_holes(m):
    outside = m.copy()
    ImageDraw.floodfill(outside, (0, 0), 128)
    return outside.point(lambda v: 0 if v == 128 else 255)


def pieces(a):
    """The connected pieces of a 0/255 mask."""
    work, out = a.copy(), []
    w, h = a.size
    for y in range(h):
        for x in range(w):
            if work.getpixel((x, y)) == 255:
                ImageDraw.floodfill(work, (x, y), 100)
                m = work.point(lambda v: 255 if v == 100 else 0)
                out.append(m)
                work = ImageChops.subtract(work, m.point(lambda v: 155 if v else 0))
    return out


def centre_piece(a, ps):
    w, h = a.size
    seed = min(((x, y) for y in range(h) for x in range(w) if a.getpixel((x, y))),
               key=lambda p: (p[0] - w / 2) ** 2 + (p[1] - h / 2) ** 2)
    return next(m for m in ps if m.getpixel(seed))


def plate(mask):
    """Bronze over the outline: light from the upper left, grain, a dark rim and a bevel."""
    w, h = mask.size
    rnd = random.Random(7)
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            t = max(0.0, 1.0 - math.hypot((x - w * 0.42) / w, (y - h * 0.38) / h) * 1.9) ** 0.8
            g = rnd.uniform(-12, 12)
            px[x, y] = (int(95 + 95 * t + g), int(62 + 70 * t + g * 0.8), int(38 + 45 * t + g * 0.6))
    edge = ImageChops.subtract(mask, mask.filter(ImageFilter.MinFilter(9)))
    img = Image.composite(Image.new("RGB", (w, h), (40, 26, 16)), img, edge)
    hi = ImageChops.subtract(mask, ImageChops.offset(mask, 3, 3)).filter(ImageFilter.GaussianBlur(1))
    img = Image.composite(Image.new("RGB", (w, h), (230, 190, 140)), img, hi.point(lambda v: v // 2))
    out = img.convert("RGBA")
    out.putalpha(mask)
    return out


def octagon(n, cut=0.22):
    m = Image.new("L", (n, n), 0)
    c = int(n * cut)
    ImageDraw.Draw(m).polygon([(c, 2), (n - c, 2), (n - 2, c), (n - 2, n - c), (n - c, n - 2), (c, n - 2),
                               (2, n - c), (2, c)], fill=255)
    return m


def rivets(img):
    d = ImageDraw.Draw(img)
    for fx, fy in ((0.3, 0.2), (0.72, 0.24), (0.25, 0.75), (0.75, 0.78)):
        x, y = N * fx, N * fy
        d.ellipse((x - 5, y - 5, x + 5, y + 5), fill=(70, 50, 35, 255))
        d.ellipse((x - 3, y - 4, x + 2, y + 1), fill=(225, 210, 185, 255))


def symbol_of(icon, a):
    """Dark ink on the icon, and holes cut through it (letters, face button symbols)."""
    ink = ImageChops.multiply(a, icon.convert("L").point(lambda v: 255 if v < 110 else 0))
    return ImageChops.lighter(ink, ImageChops.subtract(fill_holes(a), a))


def face_kind(name):
    m = re.match(r"playstation_button_color_(cross|triangle|circle|square)$", name)
    if m:
        return m.group(1)
    if re.match(r"(xbox_button_color|steamdeck_button)_[abxy]$", name):
        return "letter"
    return None


def face(icon, a, kind):
    sym = symbol_of(icon, a)
    big = sym.resize((int(N * 1.55),) * 2, Image.LANCZOS)  # the original's symbol nearly fills the plate
    o = (big.width - N) // 2
    sym = big.crop((o, o, o + N, o + N)).filter(ImageFilter.MaxFilter(3))
    mask = octagon(N)
    out = plate(mask)
    rivets(out)
    return finish(out, mask, sym, SYMBOL.get(kind, WHITE))


def shaped(icon, a, name):
    if any(k in name for k in PLATE_PIECES):
        return marked(icon, a)
    mask = fill_holes(a)
    return finish(plate(mask), mask, symbol_of(icon, a), WHITE)


def marked(icon, a):
    """Bronze with white marks: the small pieces (arrows, label letters) and the letter in the centre piece.
    The gaps between the other pieces (a stick's ring) become a light bronze line with black shading."""
    mask = fill_holes(a.filter(ImageFilter.MaxFilter(11)).filter(ImageFilter.MinFilter(11)))
    ps = pieces(a)
    marks = Image.new("L", a.size, 0)
    for m in ps:
        if m.histogram()[255] < N * N * 0.06:
            marks = ImageChops.lighter(marks, m)
    cap = centre_piece(a, ps)
    marks = ImageChops.lighter(marks, ImageChops.subtract(fill_holes(cap), cap))
    out = plate(mask)
    gaps = ImageChops.subtract(ImageChops.subtract(mask, a), marks)
    out = Image.composite(Image.new("RGBA", (N, N), RING + (255,)), out, gaps)
    shade = ImageChops.subtract(ImageChops.offset(gaps, 2, 3), gaps).filter(ImageFilter.GaussianBlur(1.2))
    shade = ImageChops.multiply(shade.point(lambda v: v * 3 // 4),
                                ImageChops.subtract(mask, marks.filter(ImageFilter.MaxFilter(9))))
    out = Image.composite(Image.new("RGBA", (N, N), (0, 0, 0, 255)), out, shade)
    return finish(out, mask, marks, WHITE)


def finish(out, mask, sym, colour):
    """The symbol in its colour with a black edge."""
    edge = sym.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.GaussianBlur(0.7))
    mask = ImageChops.lighter(mask, edge)  # (an arrow's edge may stand past the plate)
    out = Image.composite(Image.new("RGBA", (N, N), (0, 0, 0, 255)), out, edge)
    out = Image.composite(Image.new("RGBA", (N, N), colour + (255,)), out, sym)
    out.putalpha(mask)
    return out


def make(src):
    icon = Image.open(src).convert("RGBA").resize((N, N), Image.LANCZOS)
    a = binary(icon.getchannel("A"))
    name = os.path.splitext(os.path.basename(src))[0]
    kind = face_kind(name)
    return face(icon, a, kind) if kind else shaped(icon, a, name)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    pack = sys.argv[1]
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "runtime", "assets", "prompts")
    for style, folder in STYLES.items():
        out_dir = os.path.join(root, style)
        names = sorted(f for f in os.listdir(out_dir) if f.endswith(".png"))
        for f in names:
            im = make(os.path.join(pack, folder, "Double", f))
            im.quantize(256, method=Image.Quantize.FASTOCTREE).save(os.path.join(out_dir, f), optimize=True)
        print(f"{style}: {len(names)} icons")


if __name__ == "__main__":
    main()
