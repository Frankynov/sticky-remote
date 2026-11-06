#!/usr/bin/env python3
"""Rasterise the firmware's fonts into 1-bit glyph files for the simulator.

Mirrors the `font:` section of sticky-remote.yaml: same TTF files, same pixel
sizes, no anti-aliasing (ESPHome's default bpp: 1). The text fonts come from
firmware/fonts/sets.json (one folder per set); the icon fonts are the same in every set.
Output: engine/build/fonts/<set>/<id>.fnt

Format (text): "font <ascent> <line height>", then one line per glyph:
  "g <codepoint> <advance> <x offset> <y offset from line top> <w> <h> <hex bytes>"
where the bytes hold the bitmap packed MSB-first row after row, as ESPHome
stores glyphs (so both sources feed the same loader).
"""
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "engine" / "build" / "fonts"
TEXT_TTF = ROOT / "firmware" / "fonts" / "Inter-Bold.ttf"
ICON_TTF = ROOT / "firmware" / "fonts" / "materialdesignicons-webfont.ttf"

# Latin-1 + Latin Extended-A covers French, Czech, German...; plus the
# typographic extras the UI uses.
TEXT_CHARS = [chr(c) for c in range(0x20, 0x7F)] + [chr(c) for c in range(0xA0, 0x180)] + list("…·–—‘’“”")
ICONS = [
    0xF05A9, 0xF05AA, 0xF040A, 0xF03E4,                     # wifi, wifi-off, play, pause
    0xF075E, 0xF075D, 0xF0425, 0xF04B2, 0xF17B3, 0xF0502, 0xF0FCF, 0xF040E,
    0xF0AE2, 0xF0AE3, 0xF0769, 0xF17C7,                     # star, ceiling light
    0xF0335, 0xF0336, 0xF08DD, 0xF17C8,                     # bulb, floor lamp
    0xF0D2A, 0xF0D71, 0xF0143, 0xF0140, 0xF0141, 0xF0142,   # skip, chevrons
    0xF083B, 0xF07D0, 0xF16C7,                              # tv-off, home-assistant, wifi-sync
]
ICON_FONTS = {f"i{n}": (ICON_TTF, n, [chr(c) for c in ICONS]) for n in (24, 40, 52, 64, 120)}
SETS = json.loads((ROOT / "firmware" / "fonts" / "sets.json").read_text())


def fonts_for(set_name):
    text = {role: (ROOT / "firmware" / "fonts" / path, size, TEXT_CHARS)
            for role, (path, size) in SETS[set_name]["fonts"].items()}
    return {**text, **ICON_FONTS}


def rasterise(path, size, chars):
    font = ImageFont.truetype(str(path), size)
    ascent, descent = font.getmetrics()
    lines = [f"font {ascent} {ascent + descent}"]
    pad = size
    for ch in chars:
        canvas = Image.new("1", (size * 3, size * 3), 0)
        d = ImageDraw.Draw(canvas)
        d.fontmode = "1"
        d.text((pad, pad + ascent), ch, font=font, fill=1, anchor="ls")
        advance = round(font.getlength(ch))
        box = canvas.getbbox()
        if box is None:                              # space and friends
            lines.append(f"g {ord(ch)} {advance} 0 0 0 0 -")
            continue
        x0, y0, x1, y1 = box
        glyph = canvas.crop(box)
        bits = "".join("1" if glyph.getpixel((i, j)) else "0"
                       for j in range(y1 - y0) for i in range(x1 - x0))
        bits += "0" * (-len(bits) % 8)
        hexbits = "".join(f"{int(bits[i:i + 8], 2):02x}" for i in range(0, len(bits), 8))
        lines.append(f"g {ord(ch)} {advance} {x0 - pad} {y0 - pad} {x1 - x0} {y1 - y0} {hexbits}")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    icons = {}
    for set_name in (k for k in SETS if not k.startswith("_")):
        out = OUT / set_name
        out.mkdir(parents=True, exist_ok=True)
        for name, (path, size, chars) in fonts_for(set_name).items():
            if name.startswith("i"):
                icons.setdefault(name, rasterise(path, size, chars))
                text = icons[name]
            else:
                text = rasterise(path, size, chars)
            (out / f"{name}.fnt").write_text(text)
        print(f"wrote {set_name} fonts to {out}")
