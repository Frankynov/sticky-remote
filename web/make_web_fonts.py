#!/usr/bin/env python3
"""Extracts the firmware's exact glyphs from an ESPHome build (main.cpp) into
web/fonts/<set>/<role>.fnt, the format the engine's sc_font_load_fnt_text reads.
Both typefaces are in every build: Inter t18…t30, TRMNL r18…r30; icons i24…i120;
the sleep clock's digits (clock_inter, clock_trmnl).

  python3 web/make_web_fonts.py [path/to/main.cpp]
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
main = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "firmware/.esphome/build/sticky-remote/src/main.cpp"
src = main.read_text()

data = {m[1]: [int(x, 16) for x in m[2].split(",") if x.strip()]
        for m in re.finditer(r"static constexpr uint8_t (\w+)\[\] PROGMEM = \{([^}]*)\};", src)}
tables = {}
for m in re.finditer(r"static constexpr font::Glyph (\w+)\[\] PROGMEM = \{(.*)\};", src):
    tables[m[1]] = re.findall(r"\{(\d+), \((\w+) \+ (\d+)\), (-?\d+), (-?\d+), (-?\d+), (\d+), (\d+)\}", m[2])
fonts = {m[1]: (int(m[3]), int(m[4]), tables[m[2]])
         for m in re.finditer(r"new\((\w+)\) font::Font\((\w+), \d+, (\d+), (\d+),", src)}

roles = ["t18", "t20", "t22", "t26", "t30", "i24", "i40", "i52", "i64", "i120", "clock"]
for set_name, prefix in (("inter", "t"), ("trmnl", "r")):
    out = ROOT / "web" / "fonts" / set_name
    out.mkdir(parents=True, exist_ok=True)
    for role in roles:
        fid = prefix + role[1:] if role.startswith("t") else f"clock_{set_name}" if role == "clock" else role
        baseline, height, glyphs = fonts[fid]
        lines = [f"font {baseline} {height}"]
        for cp, arr, off, adv, ox, oy, w, h in glyphs:
            n = (int(w) * int(h) + 7) // 8
            b = data[arr][int(off):int(off) + n]
            lines.append(f"g {cp} {adv} {ox} {oy} {w} {h} {''.join(f'{x:02x}' for x in b) or '-'}")
        (out / f"{role}.fnt").write_text("\n".join(lines) + "\n")
    print(f"web/fonts/{set_name}: {len(roles)} fonts from {main.name}")
