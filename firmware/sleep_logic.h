#pragma once
// Sleep and wake decisions: what a button does, which way it falls asleep,
// what ends a doze, when it sleeps by itself. Plain C++ with no ESPHome or
// ESP-IDF in it, so tests/sleep_logic_test.cpp checks every case on a
// computer; the YAML only gathers the inputs and carries out the answer.

#include <cstdint>

namespace sleep_logic {

// A press within this long of starting up (deep sleep wakes by restarting)
// or of a button waking it only wakes it: it doesn't also act.
constexpr uint32_t BOOT_IGNORE_MS = 2000;
constexpr uint32_t WAKE_PRESS_MS = 1500;
// A hold that began as the waking press doesn't put it straight back to sleep.
constexpr uint32_t HOLD_AFTER_WAKE_MS = 2500;

// --- Buttons -------------------------------------------------------------------
enum class Press {
  AI,         // AI/power, released within 1.5 s
  AI_HOLD,    // AI/power, held 1.5 s
  UP,
  DOWN,
  DOWN_HOLD,  // Down held 3 s (touch calibration, development builds)
};

enum class Do {
  NOTHING,    // swallowed: the press that woke it
  WAKE,       // back to the live screen
  BUTTON,     // its normal job (remote_ui::handle_button: AI sleeps, Up/Down volume)
  SLEEP,      // asleep now
  CALIBRATE,  // touch calibration
};

struct Awake {
  uint32_t now_ms;    // millis()
  uint32_t woke_at;   // millis() when a button last woke it (0: not since boot)
  bool sleeping;      // the asleep face is up (or going up)
  bool dozing;        // light sleep between clock minutes
};

inline bool just_woke(const Awake &a) {
  return a.now_ms < BOOT_IGNORE_MS || (a.woke_at != 0 && a.now_ms - a.woke_at < WAKE_PRESS_MS);
}

inline Do on_press(const Awake &a, Press p) {
  if (a.sleeping || a.dozing) return Do::WAKE;   // any button, however short
  switch (p) {
    case Press::AI:
    case Press::UP:
    case Press::DOWN:
      return just_woke(a) ? Do::NOTHING : Do::BUTTON;
    case Press::AI_HOLD:
      if (a.now_ms < HOLD_AFTER_WAKE_MS) return Do::NOTHING;
      return a.woke_at != 0 && a.now_ms - a.woke_at <= HOLD_AFTER_WAKE_MS ? Do::NOTHING : Do::SLEEP;
    case Press::DOWN_HOLD:
      return Do::CALIBRATE;
  }
  return Do::NOTHING;
}

// --- Falling asleep ------------------------------------------------------------
// With the clock: light sleep, the time moved each minute. Without (or no
// clock to show: setup screens, time not known yet): deep sleep, the dark
// "Asleep" face. On USB neither: the face stays up, buttons live, until the
// cable goes; then it decides again.
enum class Path { DOZE, DEEP, WAIT_FOR_UNPLUG };

struct Asleep {
  bool clock_on;     // the "Clock while asleep" setting
  bool in_setup;     // a setup screen (Wi-Fi, pairing) is showing
  bool time_valid;   // the clock knows the time
  bool usb;          // USB power present
};

inline bool shows_clock(const Asleep &s) { return s.clock_on && !s.in_setup && s.time_valid; }

inline Path asleep_path(const Asleep &s) {
  if (s.usb) return Path::WAIT_FOR_UNPLUG;
  return shows_clock(s) ? Path::DOZE : Path::DEEP;
}

// The face is drawn before the path is taken: if what it shows no longer
// matches (the time became known, the setting changed), draw it again first.
inline bool redraw_before(const Asleep &s, bool face_has_clock) { return shows_clock(s) != face_has_clock; }

// --- Dozing --------------------------------------------------------------------
enum class Woke { TIMER, BUTTON, USB, OTHER, REJECTED };

// What woke the light sleep, from what the chip reports. Pin levels are read
// straight after waking; a quick tap may already be over, so a pin wake with
// USB out is a button whatever the levels say now.
enum class Cause { TIMER, GPIO, OTHER };
inline Woke classify(bool slept, Cause cause, bool usb_level) {
  if (!slept) return Woke::REJECTED;
  switch (cause) {
    case Cause::TIMER: return Woke::TIMER;
    case Cause::GPIO: return usb_level ? Woke::USB : Woke::BUTTON;
    case Cause::OTHER: return Woke::OTHER;
  }
  return Woke::OTHER;
}

enum class AfterDoze { MINUTE, WAKE, DOZE_AGAIN };
inline AfterDoze after_doze(Woke w) {
  switch (w) {
    case Woke::TIMER: return AfterDoze::MINUTE;     // move the clock, doze again
    case Woke::BUTTON:
    case Woke::USB: return AfterDoze::WAKE;         // a press, or plugged in: awake
    case Woke::OTHER:
    case Woke::REJECTED: return AfterDoze::DOZE_AGAIN;
  }
  return AfterDoze::DOZE_AGAIN;
}

// Seconds of light sleep to the next whole minute (just past it), from the
// epoch time; at most max_s, and max_s when the time isn't known.
inline uint32_t doze_seconds(int64_t epoch, uint32_t max_s) {
  if (epoch < 1600000000) return max_s;
  uint32_t s = (uint32_t) (60 - epoch % 60);
  if (s > max_s) s = max_s;
  return s == 0 ? 60 : s;
}

// --- Sleeping by itself ------------------------------------------------------
struct Idle {
  uint32_t idle_ms;          // since the last touch, press or wake
  uint32_t chosen_ms;        // the "Sleep after" setting; 0 = never
  uint32_t setup_min_ms;     // setup screens stay up at least this long
  bool usb, sleeping, in_setup, installing;
};

inline bool auto_sleep_due(const Idle &i) {
  if (i.chosen_ms == 0 || i.usb || i.sleeping || i.installing) return false;
  const uint32_t limit = i.in_setup && i.setup_min_ms > i.chosen_ms ? i.setup_min_ms : i.chosen_ms;
  return i.idle_ms > limit;
}

// Asleep, not dozing, on battery, and nothing taking it the rest of the way
// (the sleep script was stopped or never finished): send it down again.
inline bool resleep_due(bool sleeping, bool dozing, bool usb, bool sleep_running) {
  return sleeping && !dozing && !usb && !sleep_running;
}

// --- Redraws while asleep ----------------------------------------------------
// The minute changed: the clock face moves; the dark face stays as drawn.
inline bool minute_redraw(bool sleeping, bool face_has_clock) { return !sleeping || face_has_clock; }

}  // namespace sleep_logic
