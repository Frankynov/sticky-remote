#!/usr/bin/env python3
"""NOTE: kept for history. These hand-drawn mockups no longer match the firmware
(since 0.4.0); design/render_screens.py renders the real screens.

1-bit mockup of the Sticky remote UI (480x800 portrait).

Renders exactly what the panel can show: black or white pixels, no
anti-aliasing, the same Inter Bold + Material Design Icons fonts the ESPHome
firmware uses. The poster is a synthetic image dithered with Floyd-Steinberg,
like the firmware will do with the Apple TV artwork.

Usage: python3 design/mockup.py   ->  design/mockup-*.png
Geometry lives in LAYOUT below and is mirrored in remote_ui.h.
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
FONTS = HERE.parent / "firmware" / "fonts"
W, H = 480, 800
INK, PAPER = 0, 1

# --- Layout (keep in sync with remote_ui.h) --------------------------------
M = 14                       # outer margin
STATUS_H = 36                # status bar
TILE_Y, TILE_H, TILE_W, TILE_GAP = 42, 124, 140, 16
TAB_Y, TAB_H = 174, 54              # "Now playing | Remote" switch
POSTER_Y, POSTER_H = 238, 256        # 2:3 poster -> ~170 wide
TITLE_Y = 506                         # title text top
SUB_Y = 544
SEEK_Y, SEEK_H = 582, 14
TIME_Y = 602
TRANSPORT_CY = 672
VOL_Y, VOL_H = 724, 62
PAD_CX, PAD_CY, PAD_R, PAD_R_IN = 240, 404, 158, 60   # remote page d-pad
KEY_Y, KEY_H, KEY_W, KEY_GAP = 580, 72, 104, 12
MINI_Y, MINI_H = 664, 48

ICON = {
    "star_on": "\U000F0AE2", "star_off": "\U000F0AE3",
    "ceiling_on": "\U000F0769", "ceiling_off": "\U000F17C7",
    "apple": "\U000F0035", "power": "\U000F0425",
    "play": "\U000F040A", "pause": "\U000F03E4",
    "rew": "\U000F0D2A", "ff": "\U000F0D71",
    "vol_minus": "\U000F075E", "vol_plus": "\U000F075D",
    "wifi": "\U000F05A9", "tv_off": "\U000F083B", "movie": "\U000F0FCF",
    "up": "\U000F0143", "down": "\U000F0140", "left": "\U000F0141", "right": "\U000F0142",
    "back": "\U000F17B3", "tv": "\U000F0502", "play_pause": "\U000F040E",
}


def font(size):
    return ImageFont.truetype(str(FONTS / "Inter-Bold.ttf"), size)


def icons(size):
    return ImageFont.truetype(str(FONTS / "materialdesignicons-webfont.ttf"), size)


F = {s: font(s) for s in (18, 20, 22, 26, 30)}
I = {s: icons(s) for s in (24, 40, 52, 64, 120)}


def text(d, xy, s, f, fill=INK, anchor="mm"):
    d.text(xy, s, font=f, fill=fill, anchor=anchor)


def fit(d, s, f, max_w):
    """Ellipsize s so it fits in max_w pixels (firmware does the same)."""
    if d.textlength(s, font=f) <= max_w:
        return s
    while s and d.textlength(s + "…", font=f) > max_w:
        s = s[:-1]
    return s.rstrip() + "…"


def synthetic_poster(w, h):
    """A stand-in film poster: moonlit mountains, title block at the bottom."""
    big = Image.new("L", (400, 600), 0)
    d = ImageDraw.Draw(big)
    for y in range(600):                      # night sky gradient
        d.line([(0, y), (400, y)], fill=int(20 + 120 * (y / 600) ** 1.4))
    d.ellipse([230, 70, 330, 170], fill=235)  # moon
    d.ellipse([255, 80, 345, 170], fill=int(20 + 120 * (110 / 600) ** 1.4))
    ridges = [(330, 95), (380, 60), (430, 30)]
    import math
    for i, (base, shade) in enumerate(ridges):
        pts = [(0, 600)]
        for x in range(0, 401, 8):
            y = base + 40 * math.sin(x / (55 + 15 * i) + i) + 25 * math.sin(x / 23 + 2 * i)
            pts.append((x, y))
        pts.append((400, 600))
        d.polygon(pts, fill=shade)
    d.rectangle([0, 470, 400, 600], fill=12)
    t = font(54)
    d.text((200, 510), "THE LONG", font=t, fill=240, anchor="mm")
    d.text((200, 562), "NIGHT", font=t, fill=240, anchor="mm")
    small = big.resize((w, h), Image.LANCZOS)
    return small.convert("1")                  # Floyd-Steinberg


def tile(d, x, label, icon, state, on):
    r = 14
    box = [x, TILE_Y, x + TILE_W - 1, TILE_Y + TILE_H - 1]
    if on:
        d.rounded_rectangle(box, r, fill=INK)
    else:
        d.rounded_rectangle(box, r, fill=PAPER, outline=INK, width=3)
    fg = PAPER if on else INK
    cx = x + TILE_W // 2
    text(d, (cx, TILE_Y + 38), icon, I[52], fg)
    text(d, (cx, TILE_Y + 80), label, F[22], fg)
    text(d, (cx, TILE_Y + 105), state, F[18], fg)


def status_bar(d, clock="21:47", battery=78):
    text(d, (M, STATUS_H // 2), "Salon", F[20], anchor="lm")
    x = W - M
    text(d, (x, STATUS_H // 2), clock, F[20], anchor="rm")
    x -= d.textlength(clock, font=F[20]) + 14
    text(d, (x, STATUS_H // 2), f"{battery}%", F[20], anchor="rm")
    x -= d.textlength(f"{battery}%", font=F[20]) + 8
    text(d, (x, STATUS_H // 2 + 1), ICON["wifi"], I[24], anchor="rm")


def tiles(d, etoile, luminaire, atv_on):
    xs = [M, M + TILE_W + TILE_GAP, M + 2 * (TILE_W + TILE_GAP)]
    tile(d, xs[0], "Étoile", ICON["star_on" if etoile else "star_off"],
         f"{etoile} %" if etoile else "Off", bool(etoile))
    tile(d, xs[1], "Luminaire", ICON["ceiling_on" if luminaire else "ceiling_off"],
         f"{luminaire} %" if luminaire else "Off", bool(luminaire))
    tile(d, xs[2], "Apple TV", ICON["power"], "On" if atv_on else "Off", atv_on)


def volume(d, level):
    y, h = VOL_Y, VOL_H
    d.rounded_rectangle([M, y, W - M - 1, y + h - 1], h // 2, outline=INK, width=3)
    text(d, (M + 50, y + h // 2), ICON["vol_minus"], I[40])
    text(d, (W - M - 50, y + h // 2), ICON["vol_plus"], I[40])
    d.line([(M + 100, y + 10), (M + 100, y + h - 11)], fill=INK, width=2)
    d.line([(W - M - 101, y + 10), (W - M - 101, y + h - 11)], fill=INK, width=2)
    # level as 10 segments between the buttons
    # level as 10 rising segments between the buttons (integer geometry)
    n, seg, gap = 10, 17, 5
    left = (W - (n * seg + (n - 1) * gap)) // 2
    lit = round(level / 10) if level is not None else 0
    y1 = y + h // 2 + 15
    for i in range(n):
        x0 = left + i * (seg + gap)
        box = [x0, y1 - (10 + 2 * i), x0 + seg - 1, y1]
        d.rectangle(box, fill=INK) if i < lit else d.rectangle(box, outline=INK, width=2)


def fmt(sec):
    """h:mm, minute resolution: the bar only needs a refresh once a minute."""
    h, m = divmod(int(sec) // 60, 60)
    return f"{h}:{m:02d}"


def tabs(d, active):
    """Outlined track; the selected page is a pill of its own inside it."""
    x0, x1, t, mid = M, W - M, 3, W // 2
    inset = t + 3
    d.rounded_rectangle([x0, TAB_Y, x1 - 1, TAB_Y + TAB_H - 1], TAB_H // 2, outline=INK, width=t)
    ph, py = TAB_H - 2 * inset, TAB_Y + inset
    px0, px1 = (x0 + inset, mid - 2) if active == 0 else (mid + 2, x1 - inset)
    d.rounded_rectangle([px0, py, px1 - 1, py + ph - 1], ph // 2, fill=INK)
    cy = TAB_Y + TAB_H // 2
    text(d, ((x0 + mid) // 2, cy), "Now playing", F[22], PAPER if active == 0 else INK)
    text(d, ((mid + x1) // 2, cy), "Remote", F[22], PAPER if active == 1 else INK)


def remote(d, playing_now=True):
    cx, cy, r, ri = PAD_CX, PAD_CY, PAD_R, PAD_R_IN
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=INK, width=3)
    d.ellipse([cx - ri, cy - ri, cx + ri, cy + ri], fill=INK)
    text(d, (cx, cy), "OK", F[30], PAPER)
    k = (r + ri) // 2 + 4
    text(d, (cx, cy - k), ICON["up"], I[64])
    text(d, (cx, cy + k), ICON["down"], I[64])
    text(d, (cx - k, cy), ICON["left"], I[64])
    text(d, (cx + k, cy), ICON["right"], I[64])
    keys = [("Back", ICON["back"]), ("Home", ICON["tv"]), ("Infuse", ICON["movie"]),
            ("Play", ICON["play_pause"])]
    for i, (label, icon) in enumerate(keys):
        x = M + i * (KEY_W + KEY_GAP)
        d.rounded_rectangle([x, KEY_Y, x + KEY_W - 1, KEY_Y + KEY_H - 1], 16, outline=INK, width=3)
        text(d, (x + KEY_W // 2, KEY_Y + 27), icon, I[40])
        text(d, (x + KEY_W // 2, KEY_Y + 57), label, F[18])
    # mini now-playing strip: tap to open the Now playing tab
    y, h = MINI_Y, MINI_H
    if playing_now:
        text(d, (M + 2, y + 20), ICON["pause"], I[24], anchor="lm")
        title = fit(d, "The Long Night", F[20], 260)
        text(d, (M + 34, y + 20), title, F[20], anchor="lm")
        text(d, (W - M, y + 20), "1:02 / 2:14", F[18], anchor="rm")
        d.rectangle([M, y + 38, W - M - 1, y + 40], fill=INK)
        d.rectangle([M, y + 36, M + (W - 2 * M) * 3734 // 8040, y + 42], fill=INK)
    else:
        text(d, (W // 2, y + 22), "Nothing playing", F[20])


def playing(d, img, paused=False, pos=3734, dur=8040):
    pw = POSTER_H * 2 // 3
    poster = synthetic_poster(pw, POSTER_H)
    px = (W - pw) // 2
    img.paste(poster, (px, POSTER_Y))
    d.rectangle([px - 1, POSTER_Y - 1, px + pw, POSTER_Y + POSTER_H], outline=INK, width=1)
    title = fit(d, "The Long Night", F[30], W - 2 * M)
    text(d, (W // 2, TITLE_Y), title, F[30], anchor="mt")
    text(d, (W // 2, SUB_Y), "2023 · Infuse", F[20], anchor="mt")
    # seek bar: outline + filled elapsed part + knob
    x0, x1 = M + 10, W - M - 10
    d.rounded_rectangle([x0, SEEK_Y, x1, SEEK_Y + SEEK_H], SEEK_H // 2, outline=INK, width=2)
    xf = x0 + (x1 - x0) * pos / dur
    d.rounded_rectangle([x0, SEEK_Y, xf, SEEK_Y + SEEK_H], SEEK_H // 2, fill=INK)
    d.ellipse([xf - 11, SEEK_Y + SEEK_H / 2 - 11, xf + 11, SEEK_Y + SEEK_H / 2 + 11],
              fill=PAPER, outline=INK, width=4)
    text(d, (x0, TIME_Y + 12), fmt(pos), F[20], anchor="lm")
    text(d, (x1, TIME_Y + 12), "-" + fmt(dur - pos), F[20], anchor="rm")
    text(d, (W // 2, TIME_Y + 12), fmt(dur), F[18], anchor="mm")
    # transport
    cy = TRANSPORT_CY
    text(d, (W // 2 - 130, cy), ICON["rew"], I[64])
    text(d, (W // 2 + 130, cy), ICON["ff"], I[64])
    d.ellipse([W // 2 - 42, cy - 42, W // 2 + 42, cy + 42], fill=INK)
    text(d, (W // 2, cy), ICON["play" if paused else "pause"], I[52], PAPER)


def idle(d):
    cy = POSTER_Y + 110
    text(d, (W // 2, cy), ICON["movie"], I[120])
    text(d, (W // 2, cy + 100), "Nothing playing", F[30])
    text(d, (W // 2, cy + 136), "Apple TV · Home screen", F[20])
    # primary action: open Infuse
    bw, bh, by = 300, 72, 610
    d.rounded_rectangle([(W - bw) // 2, by, (W + bw) // 2, by + bh], bh // 2, fill=INK)
    text(d, (W // 2, by + bh // 2), "Open Infuse", F[26], PAPER)


def off(d):
    cy = TAB_Y + 160
    text(d, (W // 2, cy), ICON["tv_off"], I[120])
    text(d, (W // 2, cy + 100), "Apple TV is off", F[30])
    bw, bh, by = 300, 72, 610
    d.rounded_rectangle([(W - bw) // 2, by, (W + bw) // 2, by + bh], bh // 2, fill=INK)
    text(d, (W // 2 - 20, by + bh // 2), "Turn on", F[26], PAPER)
    text(d, (W // 2 + 70, by + bh // 2 + 1), ICON["power"], I[40], PAPER)


def screen(state):
    img = Image.new("1", (W, H), PAPER)
    d = ImageDraw.Draw(img)
    d.fontmode = "1"                      # no anti-aliasing, like the panel
    status_bar(d)
    if state == "playing":
        tiles(d, 0, 62, True)
        tabs(d, 0)
        playing(d, img)
        volume(d, 40)
    elif state == "remote":
        tiles(d, 0, 62, True)
        tabs(d, 1)
        remote(d)
        volume(d, 40)
    elif state == "idle":
        tiles(d, 35, 62, True)
        tabs(d, 0)
        idle(d)
        volume(d, 40)
    else:
        tiles(d, 0, 0, False)
        off(d)
    return img


if __name__ == "__main__":
    states = ["playing", "remote", "idle", "off"]
    shots = {s: screen(s) for s in states}
    for s, im in shots.items():
        im.save(HERE / f"mockup-{s}.png")
        im.resize((W * 2, H * 2), Image.NEAREST).save(HERE / f"mockup-{s}@2x.png")
    gap = 24
    sheet = Image.new("1", (len(states) * (W + gap) + gap, H + 2 * gap), PAPER)
    for i, s in enumerate(states):
        x = gap + i * (W + gap)
        sheet.paste(shots[s], (x, gap))
        ImageDraw.Draw(sheet).rectangle([x - 1, gap - 1, x + W, gap + H], outline=INK)
    sheet.save(HERE / "mockup-sheet.png")
    print("wrote", ", ".join(f"mockup-{s}.png" for s in states), "+ mockup-sheet.png")
