"""FractumMorrowind's HOME menu art (FractumSeraph/mw3ds): Fractum Halo's ring and moon (fractum-halo.png, from
FractumSeraph/OpenCE's port/assets/icon) in Morrowind's colours, the ring gold and the moon Bloodmoon's red.
    python tools/build/art/fractum/make_fractum_art.py
Writes icon.png (48 x 48: the Makefile makes mw3ds.smdh's icon from it) and tools/build/art/fractum/banner.png (256 x
128, the flat banner bannertool makes tools/build/banner.bnr from: see this folder's README). Needs Pillow and NumPy.
"""
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
GOLD = np.array([222, 178, 92], np.float32)
RED = np.array([196, 46, 34], np.float32)


def recolour(img):
    """The ring's teal to gold, the moon's grey-blue to red, the starfield kept (darkened toward Morrowind's dusk)."""
    a = np.asarray(img.convert("RGB"), np.float32) / 255.0
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    light = a.max(axis=2)
    sat = light - a.min(axis=2)
    teal = np.clip((g + b) / 2 - r, 0, 1) * 3          # the ring: green-blue over red
    teal = np.clip(teal, 0, 1)[..., None]
    grey = np.clip(1 - sat * 4, 0, 1)[..., None] * np.clip((light - 0.35) * 3, 0, 1)[..., None]   # the moon: light, grey
    out = a * (1 - teal) + (light[..., None] * GOLD / 255.0 * 1.15) * teal
    out = out * (1 - grey) + (light[..., None] * RED / 255.0 * 1.25) * grey
    out[..., 0] = out[..., 0] * 1.04 + 0.01             # (the night a little warmer)
    return Image.fromarray((np.clip(out, 0, 1) * 255).astype(np.uint8))


def main():
    art = recolour(Image.open(HERE / "fractum-halo.png"))
    icon = art.resize((48, 48), Image.LANCZOS).point(lambda v: min(255, int(v * 1.25)))
    icon.save(ROOT / "icon.png")
    # The banner: the ring on the left, the name beside it
    banner = Image.new("RGB", (256, 128), (6, 8, 16))
    rng = np.random.default_rng(7)                     # (a few stars, the same each run)
    sky = ImageDraw.Draw(banner)
    for x, y, v in zip(rng.integers(0, 256, 70), rng.integers(0, 128, 70), rng.integers(40, 150, 70)):
        sky.point((int(x), int(y)), fill=(int(v), int(v), int(v * 1.1) if v * 1.1 < 255 else 255))
    ring = art.resize((112, 112), Image.LANCZOS)
    mask = Image.new("L", (112, 112), 0)
    ImageDraw.Draw(mask).ellipse((2, 2, 110, 110), fill=255)
    banner.paste(ring, (6, 8), mask.filter(ImageFilter.GaussianBlur(3)))
    d = ImageDraw.Draw(banner)
    font = None
    for f in ("C:/Windows/Fonts/georgiab.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf"):
        if Path(f).is_file():
            font = f
            break
    big = ImageFont.truetype(font, 21) if font else ImageFont.load_default()
    small = ImageFont.truetype(font.replace("b.ttf", ".ttf") if font and font.endswith("b.ttf") else font, 12) if font else big
    for text, y, fnt, col in (("Fractum", 32, big, (222, 178, 92)), ("Morrowind", 58, big, (232, 214, 170)),
                              ("online cross-play", 90, small, (200, 128, 100))):
        d.text((125, y), text, font=fnt, fill=(0, 0, 0))
        d.text((124, y - 1), text, font=fnt, fill=col)
    banner.save(HERE / "banner.png")
    print("icon.png, tools/build/art/fractum/banner.png")


if __name__ == "__main__":
    main()
