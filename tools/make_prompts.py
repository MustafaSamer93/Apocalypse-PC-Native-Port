"""Button prompt icons in the game's style: aged bronze plates like its own legend icons.

Rebuilds runtime/assets/prompts/<style>/*.png from Kenney's "Input Prompts" pack (CC0, kenney.nl):
    python tools/make_prompts.py "<pack folder>"   (the folder with "PlayStation Series", ...)

Every icon keeps Kenney's outline. The bronze is aged, with rust in the rim and a gold bevel on the upper
left edges. Face buttons are round, the symbol in the PlayStation colours or white, all symbols as tall
as the letters; other buttons are bronze in their own shape with white letters. Sticks
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


def noise(seed, scale):
    """Blotchy noise 0..255: random values blurred to patches `scale` px across."""
    rnd = random.Random(seed)
    small = Image.new("L", (N // scale + 2,) * 2)
    small.putdata([rnd.randint(0, 255) for _ in range(small.width * small.height)])
    return small.resize((N + 2 * scale,) * 2, Image.BICUBIC).crop((scale, scale, scale + N, scale + N))


def edge_band(mask, width):
    """255 at the outline, fading to 0 about `width` px inside."""
    inner = mask.filter(ImageFilter.MinFilter(width | 1)).filter(ImageFilter.GaussianBlur(width / 2))
    return ImageChops.subtract(mask, inner)


def plate(mask, seed=3):
    """Aged bronze over the outline: light from the upper left, grain, a dark rim, rust in the rim and a
    few faint patches, and a gold bevel on the upper left edges (worn clean)."""
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
    img = Image.blend(img, Image.new("RGB", (w, h), (70, 40, 22)), 0.18)
    rim_rust = ImageChops.multiply(edge_band(mask, 10), noise(seed, 6).point(lambda v: 255 if v > 130 else 60))
    patches = ImageChops.multiply(noise(seed + 5, 12).point(lambda v: max(0, v - 170) * 3), mask)
    rust = ImageChops.lighter(rim_rust.point(lambda v: v * 3 // 5), patches.filter(ImageFilter.GaussianBlur(2)).point(lambda v: v // 2))
    img = Image.composite(Image.new("RGB", (w, h), (125, 55, 25)), img, rust)
    inset = mask.filter(ImageFilter.MinFilter(3))
    line = ImageChops.subtract(inset, inset.filter(ImageFilter.MinFilter(5))).filter(ImageFilter.GaussianBlur(0.6))
    light = Image.linear_gradient("L").rotate(45).resize((w, h)).transpose(Image.FLIP_TOP_BOTTOM)
    light = light.transpose(Image.FLIP_LEFT_RIGHT).point(lambda v: max(0, v * 2 - 200))
    img = Image.composite(Image.new("RGB", (w, h), (255, 225, 165)), img, ImageChops.multiply(line, light))
    out = img.convert("RGBA")
    out.putalpha(mask)
    return out


def disc(n):
    big = Image.new("L", (n * 4, n * 4), 0)
    m = n * 4 * 0.07  # (a little smaller than the icon: the symbol keeps its size)
    ImageDraw.Draw(big).ellipse((m, m, n * 4 - m, n * 4 - m), fill=255)
    return binary(big.resize((n, n), Image.LANCZOS))


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


LETTER_SCALE = 1.3  # the letters' size on a round plate (Kenney's are small)
letter_height = 0.0  # main(): the height that gives them, which the PlayStation shapes match


def face(icon, a, kind):
    sym = symbol_of(icon, a)
    scale = LETTER_SCALE
    if kind in SYMBOL:
        bb = sym.getbbox()
        scale = letter_height / (bb[3] - bb[1])
    big = sym.resize((int(N * scale),) * 2, Image.LANCZOS)
    o = (big.width - N) // 2
    sym = big.crop((o, o, o + N, o + N)).filter(ImageFilter.MaxFilter(3))
    mask = disc(N)
    return finish(plate(mask), mask, sym, SYMBOL.get(kind, WHITE))


def shaped(icon, a, name):
    if any(k in name for k in PLATE_PIECES):
        return marked(icon, a)
    mask = fill_holes(a)
    return finish(plate(mask), mask, symbol_of(icon, a), WHITE)


def marked(icon, a):
    """Bronze with white marks: the small pieces (arrows, label letters) and the letter in the centre piece.
    The gaps between the other pieces (a stick's ring) become a light bronze line with black shading."""
    ps = pieces(a)
    cap = fill_holes(centre_piece(a, ps))
    marks = ImageChops.subtract(cap, a)  # the letter cut out of the cap (an R's island stays bronze)
    loose = Image.new("L", a.size, 0)  # arrows, label letters
    for m in ps:
        size = m.histogram()[255]
        inside = ImageChops.subtract(m, cap).getbbox() is None
        if 8 <= size < N * N * 0.06 and not inside:  # (specks of a pixel or two are not marks)
            loose = ImageChops.lighter(loose, m)
    marks = ImageChops.lighter(marks, loose)
    # thin gaps closed (a stick's ring, also where an arrow breaks it), but only next to the body: a
    # label's letters, away from it, stay as drawn
    def close(m):
        return fill_holes(m.filter(ImageFilter.MaxFilter(11)).filter(ImageFilter.MinFilter(11)))
    body = ImageChops.subtract(a, loose)
    near_body = ImageChops.multiply(close(a), body.filter(ImageFilter.MaxFilter(13)))
    mask = ImageChops.lighter(ImageChops.lighter(close(body), loose), near_body)
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


def check_outline(src, out, name):
    """Where Kenney's icon is see-through, ours adds only white marks, black edges and shading, and a
    stick's ring line next to the button's body: no leftovers of the gap closing (once, bronze between a
    label's letters and in the P of OPTIONS)."""
    if face_kind(name):
        return  # round plates: their shape is ours
    a = binary(src.getchannel("A"))
    body = Image.new("L", a.size, 0)  # the solid parts (not small loose marks: arrows, a label's letters)
    for m in pieces(a):
        if m.histogram()[255] >= N * N * 0.06:
            body = ImageChops.lighter(body, m)
    body = body.filter(ImageFilter.MaxFilter(13))
    px = out.load()
    stray = []
    for y in range(N):
        for x in range(N):
            r, g, b, al = px[x, y]
            if al < 200 or a.getpixel((x, y)):
                continue
            white, dark = min(r, g, b) > 225, r + g + b < 330
            ring = r >= g >= b and r - b > 30 and body.getpixel((x, y))  # bronze, shaded or not, by a solid part
            if not (white or dark or ring):
                stray.append((x, y))
    if len(stray) > 4:  # (a few anti-aliased pixels at an edge)
        sys.exit(f"{name}: {len(stray)} pixels drawn where the icon is see-through, e.g. {stray[0]}")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    pack = sys.argv[1]
    global letter_height
    ref = Image.open(os.path.join(pack, STYLES["xbox"], "Double", "xbox_button_color_a.png")).convert("RGBA")
    ref = ref.resize((N, N), Image.LANCZOS)
    bb = symbol_of(ref, binary(ref.getchannel("A"))).getbbox()
    letter_height = (bb[3] - bb[1]) * LETTER_SCALE
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "runtime", "assets", "prompts")
    for style, folder in STYLES.items():
        out_dir = os.path.join(root, style)
        names = sorted(f for f in os.listdir(out_dir) if f.endswith(".png"))
        for f in names:
            src = os.path.join(pack, folder, "Double", f)
            im = make(src)
            check_outline(Image.open(src).convert("RGBA").resize((N, N), Image.LANCZOS), im, f[:-4])
            im.quantize(256, method=Image.Quantize.FASTOCTREE).save(os.path.join(out_dir, f), optimize=True)
        print(f"{style}: {len(names)} icons")


if __name__ == "__main__":
    main()
