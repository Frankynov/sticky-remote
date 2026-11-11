#!/usr/bin/env python3
"""Renders the setup page's device previews with the firmware's own drawing
code (engine/build/sim, i.e. firmware/remote_ui.h) and the exact ESPHome glyphs in
web/fonts/:

  web/previews/welcome-now.png     a film playing (welcome page)
  web/previews/welcome-remote.png  the Remote page (welcome page)
  web/previews/base-<set>.png      the Remote page without its top row, which
                                   the page draws with the visitor's own
                                   labels, icons and states (device-preview.js)
1-bit PNGs, 480×800. Run after web/make_web_fonts.py.
"""
import subprocess
import sys
import time
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "web" / "previews"
OUT.mkdir(parents=True, exist_ok=True)
TMP = ROOT / "engine" / "build"
TOP = 180   # rows the page draws itself: status bar and tiles (TILE_Y + TILE_H + 8)


def engine(font_set):
    p = subprocess.Popen([str(TMP / "sim"), str(ROOT / "web" / "fonts" / font_set)],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    def cmd(line):
        p.stdin.write(line + "\n"); p.stdin.flush()
        return p.stdout.readline()
    return p, cmd


def frame(cmd, name, crop_top=False):
    time.sleep(0.3)
    pbm = TMP / "preview.pbm"
    r = cmd(f"frame {pbm} 1 20 30 1793000000")
    assert "none" not in r, f"no frame for {name}"
    im = Image.open(pbm).convert("1")
    if crop_top:
        im.paste(1, (0, 0, 480, TOP))
    im.save(OUT / f"{name}.png", optimize=True)


poster = Image.open(ROOT / "design" / "posters" / "night.png").convert("L")
raw = TMP / "preview-poster.raw"
raw.write_bytes(poster.tobytes())

common = ["online 1", "battery 78", "volume 0.4", "light0_state on", "light0_brightness 158",
          "light1_state off", "light_icon0 floor", "light_icon1 ceiling", "light_count 2"]

# Welcome: a film playing, then the Remote page (Inter).
p, cmd = engine("inter")
cmd("labels Living room|Floor lamp|Ceiling|Open Infuse|Apple TV")
# Same order as Home Assistant sends them: state, app, then the details.
for c in common + ["atv_state playing", "app Infuse", "title The Long Night", "position 3734",
                   "duration 8040", "position_at 2026-10-26T18:00:00+00:00", "picture night"]:
    cmd("set " + c)
cmd(f"poster {raw} {poster.width} {poster.height}")
frame(cmd, "welcome-now")
cmd("tap 352 204 1793000000")               # Remote tab
frame(cmd, "welcome-remote")
p.kill()

# The Remote page, top row left blank, for each typeface.
for font_set in ("inter", "trmnl"):
    p, cmd = engine(font_set)
    cmd("labels |||Open app|Player")
    for c in common + ["atv_state idle"]:
        cmd("set " + c)
    frame(cmd, f"base-{font_set}", crop_top=True)
    p.kill()
print("previews:", ", ".join(sorted(f.name for f in OUT.glob("*.png"))))
