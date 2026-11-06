#!/usr/bin/env python3
"""Renders test configurations with the firmware's own code (engine/build/sim),
for comparison with web/device-preview.js (the browser output is compared by
the test in the browser; see README). Writes check/<name>.pbm.
"""
import json
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "engine" / "build" / "check"
OUT.mkdir(parents=True, exist_ok=True)
CASES = json.loads((ROOT / "web" / "check_cases.json").read_text())

for name, d in CASES.items():
    p = subprocess.Popen([str(ROOT / "engine/build/sim"), str(ROOT / "web/fonts" / d["set"])],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    def cmd(line):
        p.stdin.write(line + "\n"); p.stdin.flush()
        return p.stdout.readline()
    lights = d["lights"]
    cmd(f'labels {d["room"]}|{lights[0]["label"]}|{lights[1]["label"] if len(lights) > 1 else ""}|Open|{d["player"]["label"]}')
    cmd(f"set light_count {len(lights)}")
    for i, l in enumerate(lights):
        cmd(f"set light_icon{i} {l['icon']}")
        cmd(f"set light{i}_state {'unavailable' if l.get('offline') else 'on' if l['on'] else 'off'}")
        if l.get("brightness") is not None:
            cmd(f"set light{i}_brightness {l['brightness']}")
    cmd(f"set app_button {d['app']}")
    for c in ["online 1", "volume 0.4", "atv_state idle"]:
        cmd("set " + c)
    time.sleep(0.3)
    h, m = d["clock"].split(":")
    r = cmd(f"frame {OUT / (name + '.pbm')} 1 {int(h)} {int(m)} 1793000000")
    p.kill()
    print(name, r.strip())
