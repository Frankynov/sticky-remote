#!/usr/bin/env python3
"""Checks in the firmware's configuration that host tests can't reach.

- The buttons and USB sense are polled (use_interrupt: false). sticky::doze()
  reprograms those pins' interrupts for light sleep; an interrupt-driven
  binary sensor then missed releases and the AI button stopped working after
  the first doze (0.7.0 to 0.7.2).
- doze() puts the pins' interrupt types back after waking.
- The YAML takes its sleep and wake decisions from sleep_logic.h (tested by
  sleep_logic_test.cpp), not from conditions of its own.
"""
import re
import sys
from pathlib import Path

FW = Path(__file__).resolve().parent.parent / "firmware"
yaml = (FW / "sticky-remote.yaml").read_text()
device = (FW / "sticky_device.h").read_text()
failed = 0


def check(ok, what):
    global failed
    print(("ok    " if ok else "FAIL  ") + what)
    if not ok:
        failed += 1


def block(sensor_id):
    """The text of the binary_sensor entry with this id, up to the next entry."""
    m = re.search(r"\n  - platform: gpio\n    id: %s\n(.*?)(?=\n  - platform|\n\S)" % sensor_id, yaml, re.S)
    return m.group(1) if m else None


for sid in ("usb_power", "button_ai", "button_up", "button_down"):
    b = block(sid)
    check(b is not None, f"{sid}: found")
    if b is not None:
        check(re.search(r"^    use_interrupt: false$", b, re.M) is not None, f"{sid}: polled (use_interrupt: false)")

doze = device[device.find("inline Woke doze("):]
doze = doze[: doze.find("\n}\n")]
check("gpio_set_intr_type" in doze and ".int_type" in doze, "doze(): pins' interrupt types saved and put back")
check(doze.find("gpio_wakeup_disable") < doze.find("gpio_set_intr_type"), "doze(): put back after gpio_wakeup_disable")
check("sleep_logic::classify" in doze and "sleep_logic::doze_seconds" in doze, "doze(): uses sleep_logic")

for fn in ("on_press", "shows_clock", "redraw_before", "asleep_path", "after_doze", "auto_sleep_due", "resleep_due"):
    check(f"sleep_logic::{fn}" in yaml, f"YAML uses sleep_logic::{fn}")
check("sleep_logic.h" in yaml, "YAML includes sleep_logic.h")

# Old hand-written conditions that sleep_logic now owns.
for pat, what in ((r"millis\(\) < 2000", "boot press guard"), (r"woke_at\)\s*[<>]\s*\d", "wake press guard"),
                  (r"Woke::(TIMER|BUTTON|USB|OTHER|REJECTED)", "doze result handling")):
    check(re.search(pat, yaml) is None, f"YAML has no {what} of its own")

# Every button goes through the press script.
for which in range(5):
    check(re.search(r"script\.execute: \{ id: press, which: %d \}" % which, yaml) is not None, f"press {which} wired")

print(f"{'FAILED' if failed else 'OK'}: config, {failed} failed")
sys.exit(1 if failed else 0)
