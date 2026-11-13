// Every sleep and wake case, checked on a computer: tests/run.sh.
//
// Part 1 checks each decision in firmware/sleep_logic.h on its own, across
// all its inputs. Part 2 plays whole sequences on a small model of the
// device that wires those decisions together the way sticky-remote.yaml
// does (the press script, go_to_sleep, the doze loop, the timers), so the
// cases people actually hit are covered end to end: put it to sleep, doze,
// tap to wake, sleep again, unplug while asleep, and so on.

#include "../firmware/sleep_logic.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace sleep_logic;

static int g_checks = 0, g_failed = 0;
static std::string g_case;

#define CHECK(cond)                                                                   \
  do {                                                                                \
    g_checks++;                                                                       \
    if (!(cond)) {                                                                    \
      g_failed++;                                                                     \
      std::printf("FAIL  %s\n      %s:%d  %s\n", g_case.c_str(), __FILE__, __LINE__, #cond); \
    }                                                                                 \
  } while (0)

static void test_case(const char *name) { g_case = name; }

static const char *str(Do d) {
  switch (d) {
    case Do::NOTHING: return "NOTHING";
    case Do::WAKE: return "WAKE";
    case Do::BUTTON: return "BUTTON";
    case Do::SLEEP: return "SLEEP";
    case Do::CALIBRATE: return "CALIBRATE";
  }
  return "?";
}

// =============================================================================
// Part 1: each decision
// =============================================================================

static void buttons_while_asleep() {
  test_case("any button wakes it while asleep or dozing, at any time");
  for (Press p : {Press::AI, Press::AI_HOLD, Press::UP, Press::DOWN, Press::DOWN_HOLD})
    for (uint32_t now : {0u, 500u, 1999u, 2000u, 60000u, 4000000000u})
      for (uint32_t woke : {0u, 1u, now})
        for (int asleep = 0; asleep < 4; asleep++) {
          const bool sleeping = asleep & 1, dozing = asleep & 2;
          if (!sleeping && !dozing) continue;
          CHECK(on_press({now, woke, sleeping, dozing}, p) == Do::WAKE);
        }
}

static void buttons_while_awake() {
  test_case("awake, long after boot: AI acts (sleeps), Up/Down act (volume)");
  CHECK(on_press({60000, 0, false, false}, Press::AI) == Do::BUTTON);
  CHECK(on_press({60000, 0, false, false}, Press::UP) == Do::BUTTON);
  CHECK(on_press({60000, 0, false, false}, Press::DOWN) == Do::BUTTON);
  CHECK(on_press({60000, 0, false, false}, Press::AI_HOLD) == Do::SLEEP);
  CHECK(on_press({60000, 0, false, false}, Press::DOWN_HOLD) == Do::CALIBRATE);

  test_case("awake, a button woke it long ago: presses act");
  CHECK(on_press({60000, 10000, false, false}, Press::AI) == Do::BUTTON);
  CHECK(on_press({60000, 10000, false, false}, Press::AI_HOLD) == Do::SLEEP);

  test_case("the press that started it up (deep sleep restarts it) only wakes it");
  for (uint32_t now : {0u, 100u, 1999u}) {
    CHECK(on_press({now, 0, false, false}, Press::AI) == Do::NOTHING);
    CHECK(on_press({now, 0, false, false}, Press::UP) == Do::NOTHING);
    CHECK(on_press({now, 0, false, false}, Press::DOWN) == Do::NOTHING);
    CHECK(on_press({now, 0, false, false}, Press::AI_HOLD) == Do::NOTHING);
  }
  test_case("from 2 s after starting up, a press acts");
  CHECK(on_press({2000, 0, false, false}, Press::AI) == Do::BUTTON);
  CHECK(on_press({2000, 0, false, false}, Press::UP) == Do::BUTTON);

  test_case("a hold that began at start-up doesn't send it back to sleep");
  CHECK(on_press({2499, 0, false, false}, Press::AI_HOLD) == Do::NOTHING);
  CHECK(on_press({2500, 0, false, false}, Press::AI_HOLD) == Do::SLEEP);

  test_case("the press that woke it from a doze only wakes it (1.5 s)");
  const uint32_t w = 100000;
  for (uint32_t dt : {0u, 1u, 700u, 1499u}) {
    CHECK(on_press({w + dt, w, false, false}, Press::AI) == Do::NOTHING);
    CHECK(on_press({w + dt, w, false, false}, Press::UP) == Do::NOTHING);
    CHECK(on_press({w + dt, w, false, false}, Press::DOWN) == Do::NOTHING);
  }
  CHECK(on_press({w + 1500, w, false, false}, Press::AI) == Do::BUTTON);
  CHECK(on_press({w + 1500, w, false, false}, Press::UP) == Do::BUTTON);

  test_case("holding the waking press doesn't put it straight back to sleep (2.5 s)");
  CHECK(on_press({w + 1500, w, false, false}, Press::AI_HOLD) == Do::NOTHING);
  CHECK(on_press({w + 2500, w, false, false}, Press::AI_HOLD) == Do::NOTHING);
  CHECK(on_press({w + 2501, w, false, false}, Press::AI_HOLD) == Do::SLEEP);

  test_case("millis() wrapping round (49.7 days) doesn't block the buttons");
  const uint32_t near_wrap = 0xFFFFFF00u;
  CHECK(on_press({near_wrap + 0x200u, near_wrap, false, false}, Press::AI) == Do::NOTHING);   // 0x200 ms after waking
  CHECK(on_press({near_wrap + 0x1000u, near_wrap, false, false}, Press::AI) == Do::BUTTON);  // 4 s after, past the wrap
  CHECK(on_press({5000, 4000000000u, false, false}, Press::AI) == Do::BUTTON);   // woke long before the wrap
}

static void falling_asleep() {
  test_case("on battery, clock on, time known, not in setup: dozes with the clock");
  CHECK(asleep_path({true, false, true, false}) == Path::DOZE);
  CHECK(shows_clock({true, false, true, false}));

  test_case("clock off: deep sleep, dark face");
  CHECK(asleep_path({false, false, true, false}) == Path::DEEP);
  CHECK(!shows_clock({false, false, true, false}));

  test_case("clock on but in setup: deep sleep, dark face (no frozen clock)");
  CHECK(asleep_path({true, true, true, false}) == Path::DEEP);
  CHECK(!shows_clock({true, true, true, false}));

  test_case("clock on but the time isn't known yet: deep sleep, dark face");
  CHECK(asleep_path({true, false, false, false}) == Path::DEEP);
  CHECK(!shows_clock({true, false, false, false}));

  test_case("on USB: waits for the cable to go, whatever the settings");
  for (int b = 0; b < 8; b++)
    CHECK(asleep_path({bool(b & 1), bool(b & 2), bool(b & 4), true}) == Path::WAIT_FOR_UNPLUG);

  test_case("on USB, the face already shows what unplugging will lead to");
  CHECK(shows_clock({true, false, true, true}));
  CHECK(!shows_clock({false, false, true, true}));

  test_case("the face is drawn again only when it no longer matches");
  CHECK(!redraw_before({true, false, true, false}, true));
  CHECK(!redraw_before({false, false, true, false}, false));
  CHECK(redraw_before({true, false, true, false}, false));    // the time became known while on USB
  CHECK(redraw_before({false, false, true, false}, true));    // clock turned off while on USB
  CHECK(redraw_before({true, true, true, false}, true));      // a setup screen came up

  test_case("whenever it dozes, the face shows the clock; whenever it goes deep, it doesn't");
  for (int b = 0; b < 8; b++) {
    const Asleep s{bool(b & 1), bool(b & 2), bool(b & 4), false};
    CHECK((asleep_path(s) == Path::DOZE) == shows_clock(s));
  }
}

static void dozing() {
  test_case("what woke the light sleep");
  CHECK(classify(true, Cause::TIMER, false) == Woke::TIMER);
  CHECK(classify(true, Cause::TIMER, true) == Woke::TIMER);
  CHECK(classify(true, Cause::GPIO, false) == Woke::BUTTON);   // a tap already over by now still counts
  CHECK(classify(true, Cause::GPIO, true) == Woke::USB);
  CHECK(classify(true, Cause::OTHER, false) == Woke::OTHER);
  CHECK(classify(false, Cause::TIMER, false) == Woke::REJECTED);
  CHECK(classify(false, Cause::GPIO, true) == Woke::REJECTED);

  test_case("what follows a doze");
  CHECK(after_doze(Woke::TIMER) == AfterDoze::MINUTE);
  CHECK(after_doze(Woke::BUTTON) == AfterDoze::WAKE);
  CHECK(after_doze(Woke::USB) == AfterDoze::WAKE);
  CHECK(after_doze(Woke::OTHER) == AfterDoze::DOZE_AGAIN);
  CHECK(after_doze(Woke::REJECTED) == AfterDoze::DOZE_AGAIN);

  test_case("dozes to just past the next whole minute");
  const int64_t minute = 1794578520;   // a whole minute (…:00)
  CHECK(minute % 60 == 0);
  CHECK(doze_seconds(minute, 600) == 60);
  CHECK(doze_seconds(minute + 1, 600) == 59);
  CHECK(doze_seconds(minute + 59, 600) == 1);
  CHECK(doze_seconds(minute + 30, 10) == 10);    // capped
  CHECK(doze_seconds(0, 600) == 600);            // time unknown
  CHECK(doze_seconds(-5, 600) == 600);
  for (int k = 0; k < 60; k++) {
    const uint32_t s = doze_seconds(minute + k, 600);
    CHECK(s >= 1 && s <= 60 && (minute + k + s) % 60 == 0);
  }
}

static void by_itself() {
  const uint32_t MIN = 60000, SETUP = 600000;
  test_case("sleeps after the chosen quiet spell, on battery");
  CHECK(!auto_sleep_due({3 * MIN, 3 * MIN, SETUP, false, false, false, false}));   // exactly: not yet
  CHECK(auto_sleep_due({3 * MIN + 1, 3 * MIN, SETUP, false, false, false, false}));
  CHECK(!auto_sleep_due({2 * MIN, 3 * MIN, SETUP, false, false, false, false}));

  test_case("never by itself: setting 'never', on USB, already asleep, installing an update");
  CHECK(!auto_sleep_due({999 * MIN, 0, SETUP, false, false, false, false}));
  CHECK(!auto_sleep_due({999 * MIN, 3 * MIN, SETUP, true, false, false, false}));
  CHECK(!auto_sleep_due({999 * MIN, 3 * MIN, SETUP, false, true, false, false}));
  CHECK(!auto_sleep_due({999 * MIN, 3 * MIN, SETUP, false, false, false, true}));

  test_case("setup screens stay up at least 10 minutes");
  CHECK(!auto_sleep_due({5 * MIN, 3 * MIN, SETUP, false, false, true, false}));
  CHECK(auto_sleep_due({10 * MIN + 1, 3 * MIN, SETUP, false, false, true, false}));
  CHECK(!auto_sleep_due({20 * MIN, 30 * MIN, SETUP, false, false, true, false}));   // a longer choice wins
  CHECK(auto_sleep_due({30 * MIN + 1, 30 * MIN, SETUP, false, false, true, false}));

  test_case("asleep on battery with nothing taking it down: sent down again");
  CHECK(resleep_due(true, false, false, false));
  CHECK(!resleep_due(true, true, false, false));    // dozing: already down
  CHECK(!resleep_due(true, false, true, false));    // on USB: stays up on purpose
  CHECK(!resleep_due(true, false, false, true));    // the sleep script is on it
  CHECK(!resleep_due(false, false, false, false));  // awake

  test_case("minute ticks move the clock face, never the dark face");
  CHECK(minute_redraw(false, false));
  CHECK(minute_redraw(false, true));
  CHECK(minute_redraw(true, true));
  CHECK(!minute_redraw(true, false));
}

// =============================================================================
// Part 2: whole sequences, on a model of the device
// =============================================================================
//
// Mirrors sticky-remote.yaml: the press script (on_press), wake_up,
// go_to_sleep (face, USB wait, redraw check, doze or deep sleep), the doze
// loop (classify, after_doze) and the 5 s timers (auto_sleep_due,
// resleep_due). Deep sleep restarts the chip, so here it's a restart: RAM
// state cleared, millis() back to 0.

struct Device {
  // Settings and surroundings
  bool clock_on = true, in_setup = false, time_valid = true, usb = false, installing = false;
  uint32_t sleep_after_ms = 3 * 60000;
  // Firmware state
  uint32_t ms = 60000, woke_at = 0, last_activity = 0;
  bool sleeping = false, dozing = false, face_clock = false;
  bool sleep_script = false;    // go_to_sleep is running (waiting for unplug)
  bool deep = false;            // in deep sleep
  int restarts = 0, volume_steps = 0, sleeps = 0, wakes = 0;
  std::vector<std::string> log;

  void advance(uint32_t dt) {
    ms += dt;
    if (deep || dozing) return;
    tick5s();
  }

  // --- the YAML's scripts ---
  void wake_up() {
    sleep_script = false;
    dozing = false;
    sleeping = false;
    woke_at = ms;
    last_activity = ms;
    wakes++;
    log.push_back("awake");
  }
  void go_to_sleep() {
    if (sleep_script) return;   // mode: single
    sleeps++;
    sleeping = true;
    face_clock = shows_clock({clock_on, in_setup, time_valid, usb});
    log.push_back(face_clock ? "face: clock" : "face: dark");
    if (usb) {
      sleep_script = true;     // waits for unplug
      log.push_back("waiting for unplug");
      return;
    }
    finish_sleep();
  }
  void finish_sleep() {
    sleep_script = false;
    const Asleep st{clock_on, in_setup, time_valid, false};
    if (redraw_before(st, face_clock)) {
      face_clock = shows_clock(st);
      log.push_back(face_clock ? "face again: clock" : "face again: dark");
    }
    if (asleep_path({clock_on, in_setup, time_valid, usb}) == Path::DOZE) {
      dozing = true;
      log.push_back("dozing");
    } else {
      deep = true;
      log.push_back("deep sleep");
    }
  }
  void press(Press p) {
    if (deep) {   // ext1 wake: the chip restarts; this press is the one that started it
      restart();
      p = Press::AI;   // whatever it was, it arrives as an early press
    }
    if (dozing) {   // GPIO wake from light sleep
      if (after_doze(classify(true, Cause::GPIO, usb)) == AfterDoze::WAKE) wake_up();
      return;       // the press itself arrives after waking (swallowed below if still seen)
    }
    last_activity = ms;
    const Do d = on_press({ms, woke_at, sleeping, dozing}, p);
    log.push_back(std::string("press -> ") + str(d));
    switch (d) {
      case Do::NOTHING: break;
      case Do::WAKE: wake_up(); break;
      case Do::SLEEP: go_to_sleep(); break;
      case Do::CALIBRATE: break;
      case Do::BUTTON:
        if (p == Press::AI) go_to_sleep();          // remote_ui::handle_button: AI sleeps
        else if (!in_setup) volume_steps += p == Press::UP ? 1 : -1;
        break;
    }
  }
  // The release of the press that woke a doze reaches the press script afterwards.
  void tap_to_wake_from_doze(Press p) {
    press(p);
    ms += 120;
    press(p);
  }
  void plug(bool on) {
    usb = on;
    last_activity = ms;
    if (dozing && on) {   // GPIO9 high wakes the light sleep
      if (after_doze(classify(true, Cause::GPIO, true)) == AfterDoze::WAKE) wake_up();
      return;
    }
    if (deep && on) { restart(); return; }   // deep_sleep wakeup_pin, KEEP_AWAKE
    if (!on && sleep_script) {
      log.push_back("unplugged");
      finish_sleep();
    }
  }
  void minute_passes() {
    if (dozing) {
      CHECK(after_doze(classify(true, Cause::TIMER, usb)) == AfterDoze::MINUTE);
      ms += 60000;
      return;
    }
    advance(60000);
  }
  void restart() {
    restarts++;
    deep = false; sleeping = false; dozing = false; sleep_script = false;
    ms = 300; woke_at = 0; last_activity = 0;
    log.push_back("restart");
  }
  void tick5s() {
    if (auto_sleep_due({ms - last_activity, sleep_after_ms, 600000, usb, sleeping, in_setup, installing})) {
      log.push_back("quiet spell over");
      go_to_sleep();
    }
    if (resleep_due(sleeping, dozing, usb, sleep_script)) go_to_sleep();
  }
  bool awake() const { return !sleeping && !dozing && !deep; }
};

static void scenarios() {
  test_case("scenario: AI sleeps (clock on, battery), a quick tap wakes, AI sleeps again");
  {
    Device d;
    d.press(Press::AI);
    CHECK(d.dozing && d.face_clock);
    d.minute_passes();
    d.minute_passes();
    d.tap_to_wake_from_doze(Press::AI);
    CHECK(d.awake());
    d.advance(3000);
    d.press(Press::AI);                       // the bug of 0.7.2: this did nothing
    CHECK(d.dozing);
    d.tap_to_wake_from_doze(Press::UP);
    CHECK(d.awake());
    CHECK(d.volume_steps == 0);               // the waking Up didn't change the volume
    d.advance(2000);
    d.press(Press::UP);
    CHECK(d.volume_steps == 1);
  }

  test_case("scenario: sleep and wake ten times in a row, every button");
  {
    Device d;
    for (int i = 0; i < 10; i++) {
      d.press(i % 2 ? Press::AI : Press::AI_HOLD);
      CHECK(d.dozing);
      d.minute_passes();
      d.tap_to_wake_from_doze(i % 3 == 0 ? Press::AI : i % 3 == 1 ? Press::UP : Press::DOWN);
      CHECK(d.awake());
      d.advance(4000);
    }
    CHECK(d.sleeps == 10 && d.wakes == 10 && d.restarts == 0);
  }

  test_case("scenario: clock off: AI deep-sleeps with the dark face, a press restarts it awake");
  {
    Device d;
    d.clock_on = false;
    d.press(Press::AI);
    CHECK(d.deep && !d.face_clock);
    d.press(Press::DOWN);
    CHECK(d.awake() && d.restarts == 1);
    CHECK(d.volume_steps == 0);               // the waking press didn't act
    d.advance(3000);
    d.press(Press::AI);
    CHECK(d.deep);
  }

  test_case("scenario: asleep on USB, then unplugged, clock on: dozes (no frozen clock)");
  {
    Device d;
    d.usb = true;
    d.press(Press::AI);
    CHECK(d.sleeping && d.sleep_script && d.face_clock && !d.dozing && !d.deep);
    d.advance(60000);
    CHECK(d.sleeping && !d.deep);             // stays up while on USB
    d.plug(false);
    CHECK(d.dozing && !d.deep && d.face_clock);
  }

  test_case("scenario: asleep on USB, then unplugged, clock off: deep sleep");
  {
    Device d;
    d.clock_on = false;
    d.usb = true;
    d.press(Press::AI);
    CHECK(!d.face_clock && d.sleep_script);
    d.plug(false);
    CHECK(d.deep && !d.face_clock);
  }

  test_case("scenario: asleep on USB, a button wakes it; unplugging later doesn't sleep it");
  {
    Device d;
    d.usb = true;
    d.press(Press::AI);
    d.advance(5000);
    d.press(Press::UP);
    CHECK(d.awake() && !d.sleep_script);
    CHECK(d.volume_steps == 0);
    d.plug(false);
    CHECK(d.awake());
  }

  test_case("scenario: asleep on USB, clock turned off meanwhile: dark face drawn before deep sleep");
  {
    Device d;
    d.usb = true;
    d.press(Press::AI);
    CHECK(d.face_clock);
    d.clock_on = false;
    d.plug(false);
    CHECK(d.deep && !d.face_clock);
  }

  test_case("scenario: asleep on USB before the time was known; known by unplugging: clock, doze");
  {
    Device d;
    d.usb = true;
    d.time_valid = false;
    d.press(Press::AI);
    CHECK(!d.face_clock);
    d.time_valid = true;
    d.plug(false);
    CHECK(d.dozing && d.face_clock);
  }

  test_case("scenario: plugging in while dozing wakes it; it stays awake on USB");
  {
    Device d;
    d.press(Press::AI);
    CHECK(d.dozing);
    d.plug(true);
    CHECK(d.awake());
    for (int i = 0; i < 30; i++) d.advance(60000);
    CHECK(d.awake());
  }

  test_case("scenario: plugging in during deep sleep restarts it awake");
  {
    Device d;
    d.clock_on = false;
    d.press(Press::AI);
    CHECK(d.deep);
    d.plug(true);
    CHECK(d.awake() && d.restarts == 1);
  }

  test_case("scenario: quiet for the chosen time on battery: sleeps by itself, wakes on a tap");
  {
    Device d;
    d.last_activity = d.ms;
    d.advance(2 * 60000);
    CHECK(d.awake());
    d.advance(60000 + 5000);
    CHECK(d.dozing);
    d.tap_to_wake_from_doze(Press::DOWN);
    CHECK(d.awake());
    d.advance(2 * 60000);
    CHECK(d.awake());                         // the wake counts as activity
  }

  test_case("scenario: setup screen, clock on: AI sleeps deep with the dark face");
  {
    Device d;
    d.in_setup = true;
    d.press(Press::AI);
    CHECK(d.deep && !d.face_clock);
  }

  test_case("scenario: during an update install it never sleeps by itself");
  {
    Device d;
    d.installing = true;
    d.last_activity = d.ms;
    for (int i = 0; i < 60; i++) d.advance(60000);
    CHECK(d.awake());
  }

  test_case("scenario: 'never' sleep setting: stays awake on battery for hours; AI still sleeps it");
  {
    Device d;
    d.sleep_after_ms = 0;
    for (int i = 0; i < 300; i++) d.advance(60000);
    CHECK(d.awake());
    d.press(Press::AI);
    CHECK(d.dozing);
  }

  test_case("scenario: a hold of AI that woke it doesn't send it back to sleep");
  {
    Device d;
    d.press(Press::AI);
    CHECK(d.dozing);
    d.press(Press::AI);                       // pressed: wakes from the doze
    CHECK(d.awake());
    d.ms += 1500;
    d.press(Press::AI_HOLD);                  // still held at 1.5 s
    CHECK(d.awake());
  }

  test_case("scenario: AI pressed twice quickly while awake: one sleep, no wake-up loop");
  {
    Device d;
    d.press(Press::AI);
    d.ms += 200;
    d.press(Press::AI);                       // lands on the doze: wakes it
    CHECK(d.awake());
    CHECK(d.sleeps == 1 && d.wakes == 1);
  }
}

int main() {
  buttons_while_asleep();
  buttons_while_awake();
  falling_asleep();
  dozing();
  by_itself();
  scenarios();
  std::printf("%s: %d checks, %d failed\n", g_failed ? "FAILED" : "OK", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
