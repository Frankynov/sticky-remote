#!/usr/bin/env python3
"""Renders design/screens.png, the README picture, with the firmware's own
drawing code (engine/build/sim) and the exact ESPHome glyphs in web/fonts/.
Run after web/build.sh. Replaces the hand-drawn mockups (mockup.py), which
are kept for history and no longer match the firmware."""
import subprocess
import time
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
TMP = ROOT / "engine" / "build"
NOW = 1793000000
poster = Image.open(ROOT / "design" / "posters" / "night.png").convert("L")
raw = TMP / "screens-poster.raw"
raw.write_bytes(poster.tobytes())
BASE = ["online 1", "battery 78", "volume 0.4", "light_icon0 floor", "light_icon1 ceiling", "light_count 2",
        "light0_state on", "light0_brightness 158", "light1_state off", "app_button Infuse"]
PLAYING = ["atv_state playing", "app Infuse", "title The Long Night", "duration 8040", "position 3734",
           "position_at 2026-10-26T18:00:00+00:00", "picture night"]


def shot(extra, taps=(), with_poster=False):
    p = subprocess.Popen([str(TMP / "sim"), str(ROOT / "web" / "fonts" / "inter")],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    def cmd(line):
        p.stdin.write(line + "\n"); p.stdin.flush()
        return p.stdout.readline()
    cmd("labels Living room|Floor lamp|Ceiling|Open Infuse|Apple TV")
    for c in BASE + extra:
        cmd("set " + c)
    if with_poster:
        cmd(f"poster {raw} {poster.width} {poster.height}")
    for x, y in taps:
        cmd(f"tap {x} {y} {NOW}")
    time.sleep(0.35)
    pbm = TMP / "screens.pbm"
    cmd(f"frame {pbm} 1 20 30 {NOW}")
    im = Image.open(pbm).convert("L")
    p.kill()
    return im


screens = [shot(PLAYING, with_poster=True), shot(PLAYING, taps=[(352, 201)], with_poster=True),
           shot(["atv_state idle", "app Infuse"]), shot(["atv_state off"])]
gap = 24
sheet = Image.new("L", (len(screens) * 480 + (len(screens) + 1) * gap, 800 + 2 * gap), 150)
for i, im in enumerate(screens):
    sheet.paste(im, (gap + i * (480 + gap), gap))
sheet.save(ROOT / "design" / "screens.png", optimize=True)
print("wrote design/screens.png")
