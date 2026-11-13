#pragma once
// Sticky Remote: drawing, hit-testing and state for the reTerminal Sticky.
//
// Portrait 480x800, pure 1-bit. Geometry mirrors design/mockup.py: change one,
// change the other. On this panel COLOR_ON is paper (white) and COLOR_OFF is
// ink (black).

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "esphome/components/display/display.h"
#include "esphome/components/runtime_image/runtime_image.h"
#include "esphome/core/color.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"

namespace remote_ui {

using esphome::Color;
using esphome::display::BaseFont;
using esphome::display::Display;
using esphome::display::TextAlign;

// --- Layout (keep in sync with design/mockup.py) ----------------------------
constexpr int W = 480, H = 800;
constexpr int M = 14;
constexpr int STATUS_H = 36;
constexpr int TILE_Y = 42, TILE_H = 124, TILE_W = 140, TILE_GAP = 16;
// One page: tiles, the pad, the keys, what's playing, volume. No tabs, so
// nothing a tap does needs more than a partial refresh.
constexpr int PAD_CX = 240, PAD_CY = 328, PAD_R = 150, PAD_R_IN = 58;
constexpr int KEY_Y = 488, KEY_H = 70, KEY_W = 104, KEY_GAP = 12;
// What's playing: a small artwork, then title, seek bar and the -10 s / +10 s keys.
constexpr int NP_Y = 568, NP_H = 146;
constexpr int POSTER_W = 98, POSTER_H = NP_H;                 // the artwork's contain-fit box
constexpr int NP_X = M + POSTER_W + 14;                       // where the text column starts
constexpr int SEEK_Y = NP_Y + 52, SEEK_H = 14, TIME_Y = NP_Y + 79;
constexpr int SEEK_X0 = NP_X + 8, SEEK_X1 = W - M - 8;
constexpr int SKIP_Y = NP_Y + 94, SKIP_H = NP_H - 94, SKIP_GAP = 10;
constexpr int VOL_Y = 724, VOL_H = 62, VOL_BTN_W = 100;
constexpr int BIG_BTN_W = 300, BIG_BTN_H = 72, BIG_BTN_Y = 610;
constexpr float SKIP_SECONDS = 10.0f;

// The top row: the light tiles (one or two) and the player's power tile.
inline int tile_count(int lights) { return lights + 1; }
inline int tile_w(int n) { return (W - 2 * M - (n - 1) * TILE_GAP) / n; }
inline int tile_x(int i, int n) { return M + i * (tile_w(n) + TILE_GAP); }

// Material Design Icons code points (same glyph lists as the YAML fonts).
#define ICON_BULB_ON "\U000F0335"
#define ICON_BULB_OFF "\U000F0336"
#define ICON_FLOOR_ON "\U000F08DD"
#define ICON_FLOOR_OFF "\U000F17C8"
#define ICON_STAR_ON "\U000F0AE2"
#define ICON_STAR_OFF "\U000F0AE3"
#define ICON_CEILING_ON "\U000F0769"
#define ICON_CEILING_OFF "\U000F17C7"
#define ICON_POWER "\U000F0425"
#define ICON_PLAY "\U000F040A"
#define ICON_PAUSE "\U000F03E4"
#define ICON_REW "\U000F0D2A"
#define ICON_FF "\U000F0D71"
#define ICON_VOL_MINUS "\U000F075E"
#define ICON_VOL_PLUS "\U000F075D"
#define ICON_WIFI "\U000F05A9"
#define ICON_WIFI_OFF "\U000F05AA"
#define ICON_TV_OFF "\U000F083B"
#define ICON_MOVIE "\U000F0FCF"
#define ICON_SLEEP "\U000F04B2"
#define ICON_UP "\U000F0143"
#define ICON_DOWN "\U000F0140"
#define ICON_LEFT "\U000F0141"
#define ICON_RIGHT "\U000F0142"
#define ICON_BACK "\U000F17B3"
#define ICON_TV "\U000F0502"
#define ICON_PLAY_PAUSE "\U000F040E"
#define ICON_HOME_ASSISTANT "\U000F07D0"
#define ICON_WIFI_SYNC "\U000F16C7"
#define ICON_DOWNLOAD "\U000F01DA"

// --- State ------------------------------------------------------------------
struct Light {
  std::string state;          // "on" / "off" / "unavailable" / "" (unknown)
  float brightness = NAN;     // 0..255 from HA
  bool on() const { return state == "on"; }
  bool known() const { return state == "on" || state == "off"; }
  int percent() const {
    if (!on() || std::isnan(brightness)) return -1;
    return (int) lroundf(brightness * 100.0f / 255.0f);
  }
};

enum class Mode { OFF, IDLE, PLAYING };
// Network as the setup screens see it. NO_NETWORK: nothing to join yet;
// UNREACHABLE: the saved network has been out of reach for a while.
enum class Net : uint8_t { CONNECTING, CONNECTED, NO_NETWORK, UNREACHABLE };
// Which onboarding screen replaces the remote, if any.
enum class Setup : uint8_t { NONE, WIFI, CONFIGURE, HOME_ASSISTANT, CALIBRATE };
// Light tile icons, by settings index.
enum LightIcon : uint8_t { ICON_KIND_BULB = 0, ICON_KIND_CEILING = 1, ICON_KIND_FLOOR = 2, ICON_KIND_STAR = 3 };
inline uint8_t light_icon_index(const std::string &name) {
  if (name == "ceiling") return ICON_KIND_CEILING;
  if (name == "floor") return ICON_KIND_FLOOR;
  if (name == "star") return ICON_KIND_STAR;
  return ICON_KIND_BULB;
}
// What the Remote page shows as pressed (on-screen feedback for pad and keys).
enum class Press : uint8_t { NONE, UP, DOWN, LEFT, RIGHT, OK, KEY_BACK, KEY_HOME, KEY_INFUSE, KEY_PLAY, KEY_REW, KEY_FF };
// Feedback clears this long after the last press. Long enough that a quick run of
// presses moves the highlight instead of waiting behind a refresh that clears it.
constexpr uint32_t PRESS_SHOW_MS = 1500;
enum class Page { NOW, REMOTE };

struct Model {
  Light lights[2];
  std::string atv_state;      // off / standby / idle / on / playing / paused
  std::string title, series, artist, app, picture;
  float season = NAN, episode = NAN;
  float position = NAN, duration = NAN, volume = NAN;  // seconds, seconds, 0..1
  int64_t position_at = -1;   // epoch seconds when `position` was true
  bool poster_loaded = false;
  bool sleeping = false;
  bool online = false;
  float battery = NAN;
  bool usb = false;
  bool sleep_clock = true;        // asleep on battery: a clock that ticks every minute
  // Firmware updates from the GitHub release (the firmware's update entity).
  std::string fw_version;         // running
  std::string update_version;     // newer one available, or empty
  std::string update_notes;       // its changes, one per line (the manifest's summary)
  bool update_prompt = false;     // the update screen is up
  enum class Upd : uint8_t { IDLE, INSTALLING, FAILED } update_step = Upd::IDLE;
  int dow = 0, day = 0, month = 0;   // the local date, for the sleep clock (0: unknown)
  Page page = Page::REMOTE;   // always Remote since 0.5.0 (one page); kept for the engine's API
  bool state_seen = false;
  Press pressed = Press::NONE;
  uint32_t pressed_at = 0;

  // What this Sticky controls (the device's settings; the simulator sets them too).
  int light_count = 2;            // 1 or 2 light tiles
  uint8_t light_icon[2] = {3, 1}; // see LightIcon
  bool configured = true;         // false: nothing chosen yet, show "Finish setup"
  std::string app_button = "Infuse";  // the app the Remote page's app key opens

  // Onboarding
  Net net = Net::CONNECTED;
  std::string ip, device_name = "sticky-remote";
  std::string setup_url = "the Sticky setup page";   // where to finish setup (web wizard)
  bool ha_paired = true;   // the firmware loads the remembered value at boot
  bool calibrating = false; // the touch calibration screen is up

  /// No Wi-Fi to join (or none in reach): show how to set it up over USB.
  /// Connected but never added to Home Assistant: show how to add it.
  Setup setup() const {
    if (calibrating && !sleeping) return Setup::CALIBRATE;
    if (net == Net::NO_NETWORK || net == Net::UNREACHABLE) return Setup::WIFI;
    if (!configured) return Setup::CONFIGURE;
    if (net == Net::CONNECTED && !ha_paired && !online) return Setup::HOME_ASSISTANT;
    return Setup::NONE;
  }
  bool in_setup() const { return setup() != Setup::NONE; }

  Mode mode() const {
    if (atv_state == "playing" || atv_state == "paused") return Mode::PLAYING;
    if (atv_state == "on" || atv_state == "idle") return Mode::IDLE;
    return Mode::OFF;
  }
  bool atv_on() const { return mode() != Mode::OFF; }
};

inline Model &model() {
  static Model m;
  return m;
}

// HA timestamps look like 2026-11-06T08:12:33.123456+00:00. Returns epoch
// seconds, or -1 when the text does not parse.
inline int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned) (y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t) era * 146097 + (int64_t) doe - 719468;
}

inline int64_t parse_iso8601(const std::string &s) {
  int y, mo, d, h, mi, se;
  if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return -1;
  int64_t t = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
  // Offset: the last "+HH:MM" / "-HH:MM" after the time, or Z.
  const size_t tpos = s.find('T');
  const size_t sign = s.find_first_of("+-", tpos);
  if (sign != std::string::npos) {
    int oh = 0, om = 0;
    if (sscanf(s.c_str() + sign + 1, "%d:%d", &oh, &om) >= 1) {
      const int off = oh * 3600 + om * 60;
      t += s[sign] == '+' ? -off : off;
    }
  }
  return t;
}

// Position extrapolated to `now` while playing (HA only refreshes it on state
// changes). NAN when unknown.
inline float live_position(const Model &m, int64_t now_epoch) {
  if (std::isnan(m.position)) return NAN;
  float p = m.position;
  if (m.atv_state == "playing" && now_epoch > 0 && m.position_at > 0 && now_epoch > m.position_at)
    p += (float) (now_epoch - m.position_at);
  if (!std::isnan(m.duration) && m.duration > 0 && p > m.duration) p = m.duration;
  return p < 0 ? 0 : p;
}

// HA sends "None" / "unknown" for attributes that are absent.
inline std::string clean(const std::string &s) {
  if (s == "None" || s == "unknown" || s == "unavailable") return std::string();
  // Characters apps use that the fonts don't have (Infuse writes "S10 ∙ E3").
  static const char *const MAP[][2] = {
      {"\u2219", "\u00b7"}, {"\u2022", "\u00b7"}, {"\u22c5", "\u00b7"}, {"\u2027", "\u00b7"},  // dots → ·
      {"\u2014", "\u2013"}, {"\u2015", "\u2013"}, {"\u2212", "-"},                         // dashes → –, -
      {"\u2018", "'"}, {"\u2019", "'"}, {"\u201c", "\""}, {"\u201d", "\""}, {"\u00a0", " "},
      {"\u2502", "|"}, {"\uff5c", "|"}};
  std::string out = s;
  for (const auto &m : MAP) {
    const size_t from_len = strlen(m[0]);
    for (size_t pos = out.find(m[0]); pos != std::string::npos; pos = out.find(m[0], pos + strlen(m[1])))
      out.replace(pos, from_len, m[1]);
  }
  return out;
}

// Playback details HA dropped. HA forwards an attribute only when its value
// changes and never says when one disappears, so the Sticky clears them itself
// when playback stops or the app changes; whatever the next item carries then
// differs from "absent" and arrives.
inline void clear_media(Model &m, bool artwork) {
  m.title.clear();
  m.series.clear();
  m.artist.clear();
  m.season = m.episode = NAN;
  if (artwork) {
    m.position = m.duration = NAN;
    m.position_at = -1;
    m.picture.clear();
    m.poster_loaded = false;
  }
}

// --- Redraw queue ---------------------------------------------------------------
// The e-paper update is asynchronous: requests coalesce here and the YAML
// scheduler starts one refresh at a time. Everything a tap or a state change
// causes is a fast partial refresh. Partials leave ghosting, counted as
// `dirt`; the full refresh that cleans it waits until the Sticky has been
// left alone for a few seconds, so it never interrupts browsing.
struct Redraw {
  bool dirty = true, full = true;
  uint32_t requested_at = 0;
  int dirt = 0;
  uint32_t touched_at = 0;
  int drawn_minute = -2, drawn_clock = -2;
  Mode drawn_mode = Mode::OFF;
  Setup drawn_setup = Setup::NONE;
  bool drawn_once = false;
};

inline Redraw &redraw() {
  static Redraw r;
  return r;
}

inline void request(bool full = false) {
  auto &r = redraw();
  r.dirty = true;
  r.full = r.full || full;
  r.requested_at = esphome::millis();
}

/// A change that covers a large area (new artwork, the player turning on):
/// drawn with a partial refresh now, cleaned sooner.
inline void request_big() {
  redraw().dirt += 6;
  request();
}

/// A tap or a button press: holds back the cleaning refresh.
inline void touched() { redraw().touched_at = esphome::millis(); }

// --- Onboarding state (fed by the firmware's Wi-Fi/API hooks) ----------------
inline void set_network(Net net, const std::string &ip) {
  auto &m = model();
  if (net == m.net && ip == m.ip) return;
  const bool screen_changed = net != m.net || m.in_setup();
  m.net = net;
  m.ip = ip;
  if (screen_changed) request();
}

inline void set_device_name(const std::string &device) { model().device_name = device; }

inline void set_ha_paired(bool paired) {
  auto &m = model();
  if (m.ha_paired == paired) return;
  m.ha_paired = paired;
  request();
}

// For touch feedback: skip the settle window so it shows as soon as the panel is free.
inline void request_now() {
  request();
  redraw().requested_at = esphome::millis() - 1000;
}

// --- Touch calibration -----------------------------------------------------------
// Maps the touch chip's raw coordinates to screen pixels with an affine map
// fitted to taps on five targets, so it covers swapped axes, mirroring, scale
// and offset at once. Until a calibration is saved, ESPHome's own transform
// (the touch_* substitutions) is used.
struct TouchCal {
  bool valid = false;
  float ax = 0, bx = 0, cx = 0;   // x = ax*rx + bx*ry + cx
  float ay = 0, by = 0, cy = 0;   // y = ay*rx + by*ry + cy
};

inline TouchCal &touch_cal() {
  static TouchCal c;
  return c;
}

inline void map_touch(const TouchCal &c, int rx, int ry, int &x, int &y) {
  x = std::max(0, std::min(W - 1, (int) lroundf(c.ax * rx + c.bx * ry + c.cx)));
  y = std::max(0, std::min(H - 1, (int) lroundf(c.ay * rx + c.by * ry + c.cy)));
}

/// How the raw axes relate to the screen's (diagnostic: the fitted map itself is what gets stored).
inline void describe_cal(const TouchCal &c, bool &swap, bool &mirror_x, bool &mirror_y) {
  swap = std::fabs(c.bx) > std::fabs(c.ax);   // screen x follows raw y
  mirror_x = (swap ? c.bx : c.ax) < 0;
  mirror_y = (swap ? c.ay : c.by) < 0;
}

constexpr int CAL_N = 5;
constexpr int CAL_MAX_ERR = 40;   // px; worse than this and the taps didn't line up
constexpr int CAL_T[CAL_N][2] = {{50, 50}, {W - 50, 50}, {W - 50, H - 50}, {50, H - 50}, {W / 2, H / 2}};

struct Calib {
  int step = 0;                  // next target; CAL_N = fitted, test mode
  int raw[CAL_N][2] = {};
  TouchCal fit;
  float max_err = 0;
  bool have_test = false;
  int test_rx = 0, test_ry = 0, test_x = 0, test_y = 0;
};

inline Calib &calib() {
  static Calib k;
  return k;
}

/// The calibration screen is a development tool: off in released firmware
/// (the touch_calibration substitution), so users can't open it by accident.
inline bool &calibration_enabled() {
  static bool b = false;
  return b;
}

inline void calib_start() {
  if (!calibration_enabled()) return;
  calib() = Calib{};
  model().calibrating = true;
  request(true);
}

inline void calib_stop() {
  model().calibrating = false;
  request(true);
}

// Least squares for v = a*rx + b*ry + c over the taps (3x3 normal equations, Cramer).
inline bool calib_solve(const Calib &k, int axis, float &a, float &b, float &c) {
  double A[3][3] = {}, r[3] = {};
  for (int i = 0; i < CAL_N; i++) {
    const double v[3] = {(double) k.raw[i][0], (double) k.raw[i][1], 1.0};
    for (int p = 0; p < 3; p++) {
      for (int q = 0; q < 3; q++) A[p][q] += v[p] * v[q];
      r[p] += v[p] * CAL_T[i][axis];
    }
  }
  auto det = [](double m[3][3]) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  };
  const double d = det(A);
  if (std::fabs(d) < 1e-6) return false;
  double out[3];
  for (int col = 0; col < 3; col++) {
    double M2[3][3];
    for (int p = 0; p < 3; p++)
      for (int q = 0; q < 3; q++) M2[p][q] = q == col ? r[p] : A[p][q];
    out[col] = det(M2) / d;
  }
  a = (float) out[0];
  b = (float) out[1];
  c = (float) out[2];
  return true;
}

inline void calib_fit(Calib &k) {
  TouchCal f;
  f.valid = calib_solve(k, 0, f.ax, f.bx, f.cx) && calib_solve(k, 1, f.ay, f.by, f.cy);
  k.max_err = 0;
  if (f.valid) {
    for (int i = 0; i < CAL_N; i++) {
      const float ex = f.ax * k.raw[i][0] + f.bx * k.raw[i][1] + f.cx - CAL_T[i][0];
      const float ey = f.ay * k.raw[i][0] + f.by * k.raw[i][1] + f.cy - CAL_T[i][1];
      k.max_err = std::max(k.max_err, std::sqrt(ex * ex + ey * ey));
    }
    f.valid = k.max_err <= CAL_MAX_ERR;
  }
  k.fit = f;
  bool swap, mx, my;
  describe_cal(f, swap, mx, my);
  ESP_LOGI("touch", "calibration: %s, max error %.0f px, swap_xy=%d mirror_x=%d mirror_y=%d, "
           "x=%.4f*rx%+.4f*ry%+.1f y=%.4f*rx%+.4f*ry%+.1f", f.valid ? "good" : "rejected", k.max_err, swap, mx, my,
           f.ax, f.bx, f.cx, f.ay, f.by, f.cy);
}

/// A tap while calibrating, in raw touch-chip coordinates.
inline void calib_tap(int rx, int ry) {
  auto &k = calib();
  ESP_LOGI("touch", "calibration tap %d: raw x=%d y=%d", k.step + 1, rx, ry);
  if (k.step < CAL_N) {
    k.raw[k.step][0] = rx;
    k.raw[k.step][1] = ry;
    if (++k.step == CAL_N) calib_fit(k);
  } else if (k.fit.valid) {
    k.have_test = true;
    k.test_rx = rx;
    k.test_ry = ry;
    map_touch(k.fit, rx, ry, k.test_x, k.test_y);
  }
  request();
}

// Shows `p` as pressed on the Remote page; feedback_tick() clears it.
inline void press_feedback(Press p) {
  auto &m = model();
  m.pressed = p;
  m.pressed_at = esphome::millis();
  request_now();
}

// Called by the refresh scheduler: clears the pressed look once presses stop.
inline void feedback_tick() {
  auto &m = model();
  if (m.pressed != Press::NONE && esphome::millis() - m.pressed_at > PRESS_SHOW_MS) {
    m.pressed = Press::NONE;
    request_now();
  }
}

constexpr int CLEAN_AT = 16;                // partial refreshes' worth of ghosting before a cleaning one
constexpr uint32_t CLEAN_IDLE_MS = 3000;    // ...and only this long after the last touch

// Called by the scheduler when the panel is idle. Returns true when a refresh
// should start now, and whether it must be a full one.
inline bool take_refresh(const Model &m, bool &full) {
  auto &r = redraw();
  const uint32_t now = esphome::millis();
  const bool can_clean = r.dirt >= CLEAN_AT && now - r.touched_at >= CLEAN_IDLE_MS && !m.sleeping &&
                         m.pressed == Press::NONE;
  if (!r.dirty) {
    if (!can_clean) return false;
    full = true;   // nothing new to draw: just clean
  } else {
    if (now - r.requested_at < 250) return false;  // let bursts settle
    if (r.drawn_once && m.mode() != r.drawn_mode) r.dirt += 6;   // most of the lower screen changes
    full = r.full || !r.drawn_once || m.setup() != r.drawn_setup || can_clean;
  }
  r.dirt = full ? 0 : r.dirt + 1;
  r.dirty = r.full = false;
  r.drawn_mode = m.mode();
  r.drawn_setup = m.setup();
  r.drawn_once = true;
  return true;
}

// h:mm, minute resolution, so the bar needs at most one refresh a minute.
inline void fmt_hm(char *buf, size_t n, float sec) {
  if (std::isnan(sec) || sec < 0) {
    snprintf(buf, n, "--:--");
    return;
  }
  const int mins = (int) sec / 60;
  snprintf(buf, n, "%d:%02d", mins / 60, mins % 60);
}

// The minute the seek bar shows; the YAML redraws when it changes.
inline int shown_minute(const Model &m, int64_t now_epoch) {
  const float p = live_position(m, now_epoch);
  return std::isnan(p) ? -1 : (int) p / 60;
}

// --- Drawing helpers ----------------------------------------------------------
struct Fonts {
  BaseFont *t18, *t20, *t22, *t26, *t30;
  BaseFont *i24, *i40, *i52, *i64, *i120;
  BaseFont *clock;   // digits and a colon only, for the sleep clock
};

struct Ctx {
  Display &it;
  const Fonts &f;
  bool gray4 = false;   // the panel shows four grey levels (artwork dithered to them)
  Color ink = esphome::display::COLOR_OFF;
  Color paper = esphome::display::COLOR_ON;
};

inline void fill_rrect(Ctx &c, int x, int y, int w, int h, int r, Color col) {
  // Row by row: a corner pixel is in when its centre is inside the corner's
  // circle (integer maths, in half pixels). Stays exactly inside w x h, so a
  // pill's ends are true half circles with no bump.
  r = std::max(0, std::min(r, std::min(w, h) / 2));
  for (int j = 0; j < r; j++) {
    const int dy = 2 * (r - j) - 1;
    int inset = 0;
    while (inset < r && (2 * (r - inset) - 1) * (2 * (r - inset) - 1) + dy * dy > 4 * r * r) inset++;
    c.it.filled_rectangle(x + inset, y + j, w - 2 * inset, 1, col);
    c.it.filled_rectangle(x + inset, y + h - 1 - j, w - 2 * inset, 1, col);
  }
  c.it.filled_rectangle(x, y + r, w, h - 2 * r, col);
}

// Outline of `t` px: an ink rounded rect with a paper one inside.
inline void stroke_rrect(Ctx &c, int x, int y, int w, int h, int r, int t) {
  fill_rrect(c, x, y, w, h, r, c.ink);
  fill_rrect(c, x + t, y + t, w - 2 * t, h - 2 * t, std::max(0, r - t), c.paper);
}

inline int text_width(Ctx &c, BaseFont *f, const char *s) {
  int x1, y1, w, h;
  c.it.get_text_bounds(0, 0, s, f, TextAlign::TOP_LEFT, &x1, &y1, &w, &h);
  return w;
}

// Draw s centred at (x, y), shortened with an ellipsis to fit max_w.
inline void print_fit(Ctx &c, int x, int y, BaseFont *f, Color col, TextAlign a, const std::string &s,
                      int max_w) {
  std::string out = s;
  if (text_width(c, f, out.c_str()) > max_w) {
    while (!out.empty()) {
      // drop one UTF-8 code point from the end
      size_t cut = out.size() - 1;
      while (cut > 0 && (out[cut] & 0xC0) == 0x80) cut--;
      out.erase(cut);
      if (text_width(c, f, (out + "…").c_str()) <= max_w) break;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    out += "…";
  }
  c.it.print(x, y, f, col, a, out.c_str());
}

// --- Poster: contain-fit, bilinear, serpentine Floyd-Steinberg ---------------
inline const uint8_t *gray_buffer(esphome::runtime_image::RuntimeImage *img) {
  struct Access : esphome::runtime_image::RuntimeImage {
    using esphome::runtime_image::RuntimeImage::buffer_;
  };
  return static_cast<Access *>(img)->buffer_;
}

/// 8-bit grey artwork, wherever it was decoded.
struct GrayImage {
  const uint8_t *px = nullptr;
  int w = 0, h = 0;
};

// Draws the artwork centred in the box at (bx, by), POSTER_W x POSTER_H, dithered
// to black and white or to the panel's four greys; returns its drawn rectangle
// so the caller can frame it.
inline void draw_poster(Ctx &c, const GrayImage &img, int bx, int by, int &ox, int &oy, int &dw, int &dh) {
  const uint8_t *buf = img.px;
  const int sw = img.w, sh = img.h;
  dw = dh = 0;
  if (buf == nullptr || sw < 2 || sh < 2) return;
  const float scale = std::min((float) POSTER_W / sw, (float) POSTER_H / sh);
  dw = std::max(1, (int) (sw * scale));
  dh = std::max(1, (int) (sh * scale));
  ox = bx + (POSTER_W - dw) / 2;
  oy = by + (POSTER_H - dh) / 2;

  // Error rows scaled by 16, +2 guard on each side.
  static int16_t err[2][POSTER_W + 4];
  memset(err, 0, sizeof err);
  for (int y = 0; y < dh; y++) {
    int16_t *cur = err[y & 1], *nxt = err[(y + 1) & 1];
    memset(nxt, 0, sizeof err[0]);
    float sy = (y + 0.5f) / scale - 0.5f;
    sy = std::max(0.0f, std::min(sy, sh - 1.001f));
    const int y0 = (int) sy;
    const float fy = sy - y0;
    const uint8_t *r0 = buf + (size_t) y0 * sw, *r1 = r0 + sw;
    const bool ltr = (y & 1) == 0;
    for (int i = 0; i < dw; i++) {
      const int x = ltr ? i : dw - 1 - i;
      float sx = (x + 0.5f) / scale - 0.5f;
      sx = std::max(0.0f, std::min(sx, sw - 1.001f));
      const int x0 = (int) sx;
      const float fx = sx - x0;
      const float g = (1 - fy) * ((1 - fx) * r0[x0] + fx * r0[x0 + 1]) +
                      fy * ((1 - fx) * r1[x0] + fx * r1[x0 + 1]);
      const int e_i = x + 2;
      const float v = g + cur[e_i] / 16.0f;
      const bool black = v < 128.0f;
      if (c.gray4) {
        // Four greys, layered on the black-and-white dither: a partial refresh
        // keeps only each pixel's high bit, so the panel then shows exactly the
        // black-and-white version. The low bit softens it: a black dot in a
        // mid-tone becomes dark grey, a white one light grey.
        const uint8_t l8 = black ? (g >= 42.0f ? 85 : 0) : (g >= 212.0f ? 255 : 170);
        c.it.draw_pixel_at(ox + x, oy + y, Color(l8, l8, l8));
      } else {
        c.it.draw_pixel_at(ox + x, oy + y, black ? c.ink : c.paper);
      }
      const int e = (int) (v - (black ? 0.0f : 255.0f));
      const int fwd = ltr ? 1 : -1;
      cur[e_i + fwd] += e * 7;
      nxt[e_i - fwd] += e * 3;
      nxt[e_i] += e * 5;
      nxt[e_i + fwd] += e;
    }
  }
}

// --- Screen parts -------------------------------------------------------------
// Status bar. While the network comes up (seconds after each wake) the room
// name gives way to what's happening: joining Wi-Fi, then reaching HA.
inline void draw_status(Ctx &c, const Model &m, const char *room, bool time_valid, int hour, int minute) {
  const int cy = STATUS_H / 2;
  int x = W - M;
  char buf[16];
  if (time_valid) {
    snprintf(buf, sizeof buf, "%d:%02d", hour, minute);
    c.it.print(x, cy, c.f.t20, c.ink, TextAlign::CENTER_RIGHT, buf);
    x -= text_width(c, c.f.t20, buf) + 14;
  }
  if (!std::isnan(m.battery)) {
    snprintf(buf, sizeof buf, m.usb ? "%d%% +" : "%d%%", (int) m.battery);
    c.it.print(x, cy, c.f.t20, c.ink, TextAlign::CENTER_RIGHT, buf);
    x -= text_width(c, c.f.t20, buf) + 8;
  }
  const bool wifi = m.net == Net::CONNECTED;
  c.it.print(x, cy + 1, c.f.i24, c.ink, TextAlign::CENTER_RIGHT,
             wifi ? ICON_WIFI : ICON_WIFI_SYNC);   // HA state is in the text
  x -= 24 + 12;
  if (!m.update_version.empty() && wifi) {   // tap the bar: the update screen
    c.it.print(M, cy + 1, c.f.i24, c.ink, TextAlign::CENTER_LEFT, ICON_DOWNLOAD);
    print_fit(c, M + 30, cy, c.f.t20, c.ink, TextAlign::CENTER_LEFT, "Update " + m.update_version, x - M - 30);
    return;
  }
  const char *left = !wifi ? "Joining Wi-Fi…" : !m.online ? "Connecting to HA…" : room;
  print_fit(c, M, cy, c.f.t20, c.ink, TextAlign::CENTER_LEFT, left, x - M);
}

inline void draw_tile(Ctx &c, int i, int n, const char *label, const char *icon, const char *state, bool on,
                      bool known) {
  const int x = tile_x(i, n), w = tile_w(n), r = 14;
  if (on) {
    fill_rrect(c, x, TILE_Y, w, TILE_H, r, c.ink);
  } else {
    stroke_rrect(c, x, TILE_Y, w, TILE_H, r, 3);
  }
  const Color fg = on ? c.paper : c.ink;
  const int cx = x + w / 2;
  c.it.print(cx, TILE_Y + 38, c.f.i52, fg, TextAlign::CENTER, icon);
  print_fit(c, cx, TILE_Y + 80, c.f.t22, fg, TextAlign::CENTER, label, w - 12);
  c.it.print(cx, TILE_Y + 105, c.f.t18, fg, TextAlign::CENTER, known ? state : "…");
}

inline const char *light_icon(uint8_t kind, bool on) {
  switch (kind) {
    case ICON_KIND_CEILING: return on ? ICON_CEILING_ON : ICON_CEILING_OFF;
    case ICON_KIND_FLOOR: return on ? ICON_FLOOR_ON : ICON_FLOOR_OFF;
    case ICON_KIND_STAR: return on ? ICON_STAR_ON : ICON_STAR_OFF;
    default: return on ? ICON_BULB_ON : ICON_BULB_OFF;
  }
}

inline void draw_light_tile(Ctx &c, int i, int n, const Light &l, const char *label, uint8_t icon) {
  char buf[16];
  const int pct = l.percent();
  if (l.on())
    pct >= 0 ? snprintf(buf, sizeof buf, "%d %%", pct) : snprintf(buf, sizeof buf, "On");
  else
    snprintf(buf, sizeof buf, l.state == "unavailable" ? "Offline" : "Off");
  draw_tile(c, i, n, label, light_icon(icon, l.on()), buf, l.on(), l.known() || l.state == "unavailable");
}

inline void draw_tiles(Ctx &c, const Model &m, const char *name1, const char *name2, const char *player) {
  const int n = tile_count(m.light_count);
  draw_light_tile(c, 0, n, m.lights[0], name1, m.light_icon[0]);
  if (m.light_count > 1) draw_light_tile(c, 1, n, m.lights[1], name2, m.light_icon[1]);
  draw_tile(c, n - 1, n, player, ICON_POWER, m.atv_on() ? "On" : "Off", m.atv_on(), !m.atv_state.empty());
}

inline void draw_volume(Ctx &c, const Model &m) {
  const int y = VOL_Y, h = VOL_H, cy = y + h / 2;
  stroke_rrect(c, M, y, W - 2 * M, h, h / 2, 3);
  c.it.print(M + 50, cy, c.f.i40, c.ink, TextAlign::CENTER, ICON_VOL_MINUS);
  c.it.print(W - M - 50, cy, c.f.i40, c.ink, TextAlign::CENTER, ICON_VOL_PLUS);
  c.it.filled_rectangle(M + VOL_BTN_W - 1, y + 10, 2, h - 20, c.ink);
  c.it.filled_rectangle(W - M - VOL_BTN_W - 1, y + 10, 2, h - 20, c.ink);
  if (std::isnan(m.volume)) {
    c.it.print(W / 2, cy, c.f.t22, c.ink, TextAlign::CENTER, "Volume");
    return;
  }
  constexpr int n = 10, seg = 17, gap = 5;
  const int left = (W - (n * seg + (n - 1) * gap)) / 2;
  const int lit = (int) lroundf(m.volume * n);
  const int y1 = cy + 15;
  for (int i = 0; i < n; i++) {
    const int x0 = left + i * (seg + gap), hh = 10 + 2 * i;
    if (i < lit) {
      c.it.filled_rectangle(x0, y1 - hh, seg, hh + 1, c.ink);
    } else {
      c.it.filled_rectangle(x0, y1 - hh, seg, hh + 1, c.ink);
      c.it.filled_rectangle(x0 + 2, y1 - hh + 2, seg - 4, hh - 3, c.paper);
    }
  }
}

inline void draw_big_button(Ctx &c, const char *label, const char *icon) {
  const int x = (W - BIG_BTN_W) / 2;
  fill_rrect(c, x, BIG_BTN_Y, BIG_BTN_W, BIG_BTN_H, BIG_BTN_H / 2, c.ink);
  const int cy = BIG_BTN_Y + BIG_BTN_H / 2;
  if (icon == nullptr) {
    c.it.print(W / 2, cy, c.f.t26, c.paper, TextAlign::CENTER, label);
    return;
  }
  const int tw = text_width(c, c.f.t26, label), gap = 16, iw = 40;
  const int x0 = (W - (tw + gap + iw)) / 2;
  c.it.print(x0, cy, c.f.t26, c.paper, TextAlign::CENTER_LEFT, label);
  c.it.print(x0 + tw + gap, cy + 1, c.f.i40, c.paper, TextAlign::CENTER_LEFT, icon);
}

inline std::string subtitle(const Model &m) {
  char buf[48] = "";
  std::string s;
  if (!m.series.empty()) {
    s = m.series;
    if (!std::isnan(m.season) && !std::isnan(m.episode)) {
      snprintf(buf, sizeof buf, " · S%d E%d", (int) m.season, (int) m.episode);
      s += buf;
    }
  } else if (!m.artist.empty()) {
    s = m.artist;
  }
  if (!m.app.empty()) s += s.empty() ? m.app : " · " + m.app;
  return s;
}

inline void draw_off(Ctx &c) {
  const int cy = 334;
  c.it.print(W / 2, cy, c.f.i120, c.ink, TextAlign::CENTER, ICON_TV_OFF);
  c.it.print(W / 2, cy + 100, c.f.t30, c.ink, TextAlign::CENTER, "Apple TV is off");
  draw_big_button(c, "Turn on", ICON_POWER);
}

inline int key_x(int i) { return M + i * (KEY_W + KEY_GAP); }
inline int skip_w() { return (W - M - NP_X - SKIP_GAP) / 2; }
inline int skip_x(int i) { return NP_X + i * (skip_w() + SKIP_GAP); }

// What's playing, under the keys: artwork, title, a seek bar to tap, -10 s / +10 s.
inline void draw_now(Ctx &c, const Model &m, const GrayImage &poster, int64_t now_epoch) {
  if (m.mode() != Mode::PLAYING) {
    const int cy = NP_Y + NP_H / 2;
    c.it.print(W / 2, cy - 14, c.f.t22, c.ink, TextAlign::CENTER, "Nothing playing");
    const std::string where = m.app.empty() ? std::string("Home screen") : m.app;
    print_fit(c, W / 2, cy + 14, c.f.t18, c.ink, TextAlign::CENTER, where, W - 2 * M);
    return;
  }
  int ox = 0, oy = 0, dw = 0, dh = 0;
  if (m.poster_loaded && poster.px != nullptr) draw_poster(c, poster, M, NP_Y, ox, oy, dw, dh);
  if (dw > 0) {
    c.it.rectangle(ox - 1, oy - 1, dw + 2, dh + 2, c.ink);
  } else {
    stroke_rrect(c, M, NP_Y, POSTER_W, NP_H, 10, 2);
    c.it.print(M + POSTER_W / 2, NP_Y + NP_H / 2, c.f.i40, c.ink, TextAlign::CENTER, ICON_MOVIE);
  }

  const int tw = W - M - NP_X;
  print_fit(c, NP_X, NP_Y + 11, c.f.t22, c.ink, TextAlign::CENTER_LEFT,
            m.title.empty() ? std::string("Playing") : m.title, tw);
  print_fit(c, NP_X, NP_Y + 34, c.f.t18, c.ink, TextAlign::CENTER_LEFT, subtitle(m), tw);

  // Seek bar: tap anywhere on it to jump there.
  const float pos = live_position(m, now_epoch);
  const bool seekable = !std::isnan(pos) && !std::isnan(m.duration) && m.duration > 0;
  const bool playing = m.atv_state == "playing";
  stroke_rrect(c, SEEK_X0, SEEK_Y, SEEK_X1 - SEEK_X0, SEEK_H, SEEK_H / 2, 2);
  const int scy = SEEK_Y + SEEK_H / 2;
  if (seekable) {
    const int xf = SEEK_X0 + (int) ((SEEK_X1 - SEEK_X0) * std::min(1.0f, pos / m.duration));
    if (xf - SEEK_X0 > SEEK_H) fill_rrect(c, SEEK_X0, SEEK_Y, xf - SEEK_X0, SEEK_H, SEEK_H / 2, c.ink);
    c.it.filled_circle(xf, scy, 10, c.ink);
    c.it.filled_circle(xf, scy, 6, c.paper);
    char a[12], b[12];
    fmt_hm(a, sizeof a, pos);
    fmt_hm(b + 1, sizeof b - 1, m.duration - pos + 59.0f);  // remaining rounds up
    b[0] = '-';
    c.it.print(NP_X, TIME_Y, c.f.t18, c.ink, TextAlign::CENTER_LEFT, a);
    c.it.print(W - M, TIME_Y, c.f.t18, c.ink, TextAlign::CENTER_RIGHT, b);
  }
  // Between the times: whether playback is live, so a bar that moves once a
  // minute reads as "playing", not "stuck".
  c.it.print((NP_X + W - M) / 2, TIME_Y + 1, c.f.i24, c.ink, TextAlign::CENTER, playing ? ICON_PLAY : ICON_PAUSE);

  static const char *const icons[2] = {ICON_REW, ICON_FF};
  static const Press keys[2] = {Press::KEY_REW, Press::KEY_FF};
  for (int i = 0; i < 2; i++) {
    const int x = skip_x(i), w = skip_w();
    const bool down = m.pressed == keys[i];
    if (down) fill_rrect(c, x, SKIP_Y, w, SKIP_H, 12, c.ink);
    else stroke_rrect(c, x, SKIP_Y, w, SKIP_H, 12, 3);
    c.it.print(x + w / 2, SKIP_Y + SKIP_H / 2, c.f.i40, down ? c.paper : c.ink, TextAlign::CENTER, icons[i]);
  }
}

// One quarter of the pad ring filled with ink: the pressed arrow. Wedges keep
// a small gap along the diagonals so the four directions read as separate.
inline void fill_wedge(Ctx &c, Press dir) {
  const int cx = PAD_CX, cy = PAD_CY, r_out = PAD_R - 2, r_in = PAD_R_IN + 8, gap = 5;
  for (int dy = -r_out; dy <= r_out; dy++) {
    for (int dx = -r_out; dx <= r_out; dx++) {
      const int d2 = dx * dx + dy * dy;
      if (d2 > r_out * r_out || d2 < r_in * r_in) continue;
      const int ax = std::abs(dx), ay = std::abs(dy);
      bool on = false;
      switch (dir) {
        case Press::UP: on = dy < 0 && ax <= ay - gap; break;
        case Press::DOWN: on = dy > 0 && ax <= ay - gap; break;
        case Press::LEFT: on = dx < 0 && ay <= ax - gap; break;
        case Press::RIGHT: on = dx > 0 && ay <= ax - gap; break;
        default: break;
      }
      if (on) c.it.draw_pixel_at(cx + dx, cy + dy, c.ink);
    }
  }
}

inline void draw_remote(Ctx &c, const Model &m, int64_t now_epoch) {
  const int cx = PAD_CX, cy = PAD_CY;
  const Press p = m.pressed;
  c.it.filled_circle(cx, cy, PAD_R, c.ink);
  c.it.filled_circle(cx, cy, PAD_R - 3, c.paper);
  const bool arrow = p == Press::UP || p == Press::DOWN || p == Press::LEFT || p == Press::RIGHT;
  if (arrow) fill_wedge(c, p);
  c.it.filled_circle(cx, cy, PAD_R_IN, c.ink);
  if (p == Press::OK) c.it.filled_circle(cx, cy, PAD_R_IN - 5, c.paper);  // pressed: ring
  c.it.print(cx, cy, c.f.t30, p == Press::OK ? c.ink : c.paper, TextAlign::CENTER, "OK");
  const int k = (PAD_R + PAD_R_IN) / 2 + 4;
  c.it.print(cx, cy - k, c.f.i64, p == Press::UP ? c.paper : c.ink, TextAlign::CENTER, ICON_UP);
  c.it.print(cx, cy + k, c.f.i64, p == Press::DOWN ? c.paper : c.ink, TextAlign::CENTER, ICON_DOWN);
  c.it.print(cx - k, cy, c.f.i64, p == Press::LEFT ? c.paper : c.ink, TextAlign::CENTER, ICON_LEFT);
  c.it.print(cx + k, cy, c.f.i64, p == Press::RIGHT ? c.paper : c.ink, TextAlign::CENTER, ICON_RIGHT);

  const char *const labels[4] = {"Back", "Home", m.app_button.empty() ? "App" : m.app_button.c_str(), "Play"};
  static const char *const icons[4] = {ICON_BACK, ICON_TV, ICON_MOVIE, ICON_PLAY_PAUSE};
  static const Press keys[4] = {Press::KEY_BACK, Press::KEY_HOME, Press::KEY_INFUSE, Press::KEY_PLAY};
  for (int i = 0; i < 4; i++) {
    const int x = key_x(i);
    const bool down = p == keys[i];
    if (down) fill_rrect(c, x, KEY_Y, KEY_W, KEY_H, 16, c.ink);
    else stroke_rrect(c, x, KEY_Y, KEY_W, KEY_H, 16, 3);
    const Color fg = down ? c.paper : c.ink;
    c.it.print(x + KEY_W / 2, KEY_Y + 24, c.f.i40, fg, TextAlign::CENTER, icons[i]);
    print_fit(c, x + KEY_W / 2, KEY_Y + 54, c.f.t18, fg, TextAlign::CENTER, labels[i], KEY_W - 8);
  }
}

// Draws `text` word-wrapped from (x, y) within max_w; returns the y after it.
inline int print_wrapped(Ctx &c, int x, int y, BaseFont *f, Color col, const std::string &text, int max_w,
                         int line_h) {
  std::string line, word;
  auto flush = [&]() {
    if (line.empty()) return;
    c.it.print(x, y, f, col, TextAlign::TOP_LEFT, line.c_str());
    y += line_h;
    line.clear();
  };
  for (size_t i = 0; i <= text.size(); i++) {
    const char ch = i < text.size() ? text[i] : ' ';
    if (ch != ' ') {
      word += ch;
      continue;
    }
    if (word.empty()) continue;
    const std::string trial = line.empty() ? word : line + " " + word;
    if (text_width(c, f, trial.c_str()) > max_w && !line.empty()) {
      flush();
      line = word;
    } else {
      line = trial;
    }
    word.clear();
  }
  flush();
  return y;
}

// A numbered step: a filled circle with the number, then wrapped text.
inline int draw_step(Ctx &c, int y, int n, const std::string &text) {
  const int r = 15, cx = M + r + 2;
  c.it.filled_circle(cx, y + 12, r, c.ink);
  char num[4];
  snprintf(num, sizeof num, "%d", n);
  c.it.print(cx, y + 12, c.f.t20, c.paper, TextAlign::CENTER, num);
  return print_wrapped(c, M + 2 * r + 16, y, c.f.t20, c.ink, text, W - 2 * M - 2 * r - 16, 27) + 14;
}

inline void draw_setup_wifi(Ctx &c, const Model &m) {
  const bool lost = m.net == Net::UNREACHABLE;
  c.it.print(W / 2, 30, c.f.t30, c.ink, TextAlign::TOP_CENTER, lost ? "Wi-Fi out of reach" : "Set up Wi-Fi");
  print_wrapped(c, M + 6, 76, c.f.t20, c.ink,
                lost ? "The Sticky can't find the network it knows. To pick another one:"
                     : "The Sticky has no Wi-Fi network yet. To give it one:",
                W - 2 * M - 12, 27);
  int y = 150;
  y = draw_step(c, y, 1, "Plug it into a computer with a USB-C data cable.");
  y = draw_step(c, y, 2, "In Chrome or Edge, open");
  stroke_rrect(c, M + 30, y, W - 2 * M - 60, 54, 14, 3);
  print_fit(c, W / 2, y + 27, c.f.t26, c.ink, TextAlign::CENTER, m.setup_url, W - 2 * M - 80);
  y += 74;
  y = draw_step(c, y, 3, "Connect the Sticky there: the page lists the networks it sees, and checks that it joins yours.");
  print_wrapped(c, M + 6, std::max(y + 10, 690), c.f.t18, c.ink, "This screen goes away once it's on Wi-Fi.", W - 2 * M - 12, 24);
}

inline void draw_setup_ha(Ctx &c, const Model &m) {
  c.it.print(W / 2, 30, c.f.t30, c.ink, TextAlign::TOP_CENTER, "Add to Home Assistant");
  c.it.print(W / 2, 150, c.f.i120, c.ink, TextAlign::CENTER, ICON_HOME_ASSISTANT);
  std::string where = "Connected to Wi-Fi";
  if (!m.ip.empty()) where += " · " + m.ip;
  c.it.print(W / 2, 232, c.f.t20, c.ink, TextAlign::TOP_CENTER, where.c_str());
  int y = 290;
  y = draw_step(c, y, 1, "In Home Assistant, open Settings, then Devices & services.");
  y = draw_step(c, y, 2, "Under Discovered, configure “" + m.device_name + "” (ESPHome).");
  // No key to enter: Home Assistant creates the encryption key when it adds the Sticky.
  y = draw_step(c, y, 3, "In the device's options, allow it to perform Home Assistant actions.");
  print_wrapped(c, M + 6, std::max(y + 10, 690), c.f.t18, c.ink, "This screen goes away once Home Assistant connects.", W - 2 * M - 12, 24);
}

// On Wi-Fi but nothing chosen yet (a fresh, generic firmware): say where to
// pick the player and lights.
inline void draw_setup_configure(Ctx &c, const Model &m) {
  c.it.print(W / 2, 30, c.f.t30, c.ink, TextAlign::TOP_CENTER, "Finish setup");
  c.it.print(W / 2, 150, c.f.i120, c.ink, TextAlign::CENTER, ICON_HOME_ASSISTANT);
  std::string where = m.net == Net::CONNECTED ? "Connected to Wi-Fi" : "Not on Wi-Fi yet";
  if (!m.ip.empty()) where += " · " + m.ip;
  c.it.print(W / 2, 232, c.f.t20, c.ink, TextAlign::TOP_CENTER, where.c_str());
  print_wrapped(c, M + 6, 278, c.f.t20, c.ink, "Choose what this Sticky controls: your player, the app to open and one or two lights.", W - 2 * M - 12, 27);
  int y = 370;
  y = draw_step(c, y, 1, "Connect the Sticky to a computer with its USB-C cable.");
  y = draw_step(c, y, 2, "In Chrome or Edge, open " + m.setup_url + ".");
  y = draw_step(c, y, 3, "Follow the steps: it sends your choices over the cable.");
  print_wrapped(c, M + 6, std::max(y + 10, 700), c.f.t18, c.ink, m.device_name, W - 2 * M - 12, 24);
}

// Asleep before setup finished (10 min on battery): nothing can reach it while it
// sleeps, so say how to bring it back instead of leaving a QR code up.
inline void draw_setup_sleep(Ctx &c, const Model &m) {
  const bool wifi = m.setup() == Setup::WIFI;
  const bool configure = m.setup() == Setup::CONFIGURE;
  c.it.print(W / 2, 200, c.f.i120, c.ink, TextAlign::CENTER, ICON_SLEEP);
  c.it.print(W / 2, 300, c.f.t30, c.ink, TextAlign::TOP_CENTER, "Asleep");
  print_wrapped(c, M + 30, 360, c.f.t22, c.ink,
                wifi ? "Wi-Fi isn't set up yet." : configure ? "Setup isn't finished yet." : "Not added to Home Assistant yet.",
                W - 2 * M - 60, 30);
  print_wrapped(c, M + 30, 420, c.f.t20, c.ink,
                wifi        ? "Press any button: the Sticky wakes up and shows how to set up Wi-Fi again."
                : configure ? "Press any button: the Sticky wakes up and shows how to finish setup."
                            : "Press any button: the Sticky wakes up and shows how to add it again.",
                W - 2 * M - 60, 28);
  print_wrapped(c, M + 30, 560, c.f.t18, c.ink, "Plugged into USB, it stays awake during setup.", W - 2 * M - 60, 24);
}

// The face left on the panel while the Sticky sleeps. Dark, and nothing like
// the live screen: tiles and playback shown there would be stale, and would
// look as if they still answered a tap. With the sleep clock on, the time
// takes the middle and ticks once a minute (the firmware dozes in between).
static const char *const DAY_NAMES[] = {"", "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const MONTH_NAMES[] = {"", "January", "February", "March", "April", "May", "June", "July",
                                          "August", "September", "October", "November", "December"};

inline void draw_asleep(Ctx &c, const Model &m, const char *room, bool time_valid, int hour, int minute) {
  c.it.fill(c.ink);
  const bool clock = m.sleep_clock && time_valid;
  char buf[48];
  if (clock) {
    snprintf(buf, sizeof buf, "%d:%02d", hour, minute);
    c.it.print(W / 2, 330, c.f.clock, c.paper, TextAlign::CENTER, buf);
    if (m.month >= 1 && m.month <= 12 && m.dow >= 1 && m.dow <= 7) {
      snprintf(buf, sizeof buf, "%s %d %s", DAY_NAMES[m.dow], m.day, MONTH_NAMES[m.month]);
      c.it.print(W / 2, 448, c.f.t26, c.paper, TextAlign::CENTER, buf);
    }
    c.it.print(W / 2, H - 92, c.f.t18, c.paper, TextAlign::CENTER, "Asleep · press any button to wake");
  } else {
    c.it.print(W / 2, 290, c.f.i120, c.paper, TextAlign::CENTER, ICON_SLEEP);
    c.it.print(W / 2, 390, c.f.t30, c.paper, TextAlign::CENTER, "Asleep");
    c.it.print(W / 2, 436, c.f.t20, c.paper, TextAlign::CENTER, "Press any button to wake");
  }
  // What was true when it fell asleep, said as such.
  std::string foot = room;
  if (time_valid && !clock) {
    snprintf(buf, sizeof buf, "%ssince %d:%02d", foot.empty() ? "" : " · ", hour, minute);
    foot += buf;
  }
  if (!std::isnan(m.battery)) {
    snprintf(buf, sizeof buf, "%sbattery %d %%", foot.empty() ? "" : " · ", (int) m.battery);
    foot += buf;
  }
  print_fit(c, W / 2, H - 44, c.f.t18, c.paper, TextAlign::CENTER, foot, W - 2 * M - 20);
}

inline void draw_cal_target(Ctx &c, int x, int y) {
  c.it.filled_circle(x, y, 24, c.ink);
  c.it.filled_circle(x, y, 20, c.paper);
  c.it.filled_rectangle(x - 34, y - 1, 69, 3, c.ink);
  c.it.filled_rectangle(x - 1, y - 34, 3, 69, c.ink);
  c.it.filled_circle(x, y, 5, c.ink);
}

inline void draw_calibrate(Ctx &c) {
  const auto &k = calib();
  char buf[96];
  c.it.print(W / 2, 120, c.f.t30, c.ink, TextAlign::TOP_CENTER, "Touch calibration");
  if (k.step < CAL_N) {
    snprintf(buf, sizeof buf, "Tap the centre of the cross · %d of %d", k.step + 1, CAL_N);
    print_wrapped(c, M + 30, 176, c.f.t20, c.ink, buf, W - 2 * M - 60, 27);
    print_wrapped(c, M + 30, 520, c.f.t18, c.ink, "Up: start again · Down: cancel", W - 2 * M - 60, 24);
    draw_cal_target(c, CAL_T[k.step][0], CAL_T[k.step][1]);
    return;
  }
  bool swap, mx, my;
  describe_cal(k.fit, swap, mx, my);
  int y = 176;
  if (!k.fit.valid) {
    snprintf(buf, sizeof buf, "The taps didn't line up (off by %d px). Press Up to start again, or Down to cancel.",
             (int) k.max_err);
    print_wrapped(c, M + 30, y, c.f.t20, c.ink, buf, W - 2 * M - 60, 27);
    return;
  }
  y = print_wrapped(c, M + 30, y, c.f.t20, c.ink, "Tap anywhere: a dot should land under your finger.", W - 2 * M - 60, 27);
  y = print_wrapped(c, M + 30, y + 10, c.f.t22, c.ink, "AI: save · Up: again · Down: cancel", W - 2 * M - 60, 28);
  snprintf(buf, sizeof buf, "swap_xy %s, mirror_x %s, mirror_y %s", swap ? "yes" : "no", mx ? "yes" : "no",
           my ? "yes" : "no");
  y = print_wrapped(c, M + 30, y + 14, c.f.t18, c.ink, buf, W - 2 * M - 60, 24);
  snprintf(buf, sizeof buf, "Error %d px. Raw x=%d y=%d at the first cross.", (int) k.max_err, k.raw[0][0], k.raw[0][1]);
  y = print_wrapped(c, M + 30, y, c.f.t18, c.ink, buf, W - 2 * M - 60, 24);
  if (k.have_test) {
    snprintf(buf, sizeof buf, "Last tap: raw %d, %d -> screen %d, %d", k.test_rx, k.test_ry, k.test_x, k.test_y);
    print_wrapped(c, M + 30, y + 6, c.f.t18, c.ink, buf, W - 2 * M - 60, 24);
    c.it.filled_circle(k.test_x, k.test_y, 12, c.ink);
    c.it.filled_circle(k.test_x, k.test_y, 4, c.paper);
  }
}

// --- Firmware update screen ----------------------------------------------------
constexpr int UPD_INSTALL_Y = 560, UPD_CLOSE_Y = 652, UPD_BTN_H = 72;

/// Room to install: on USB, or a battery that can take a minute of Wi-Fi and flash writes.
inline bool update_can_install(const Model &m) { return m.usb || std::isnan(m.battery) || m.battery >= 30; }

inline void draw_update(Ctx &c, const Model &m) {
  const bool busy = m.update_step == Model::Upd::INSTALLING;
  c.it.print(W / 2, 34, c.f.t30, c.ink, TextAlign::TOP_CENTER, busy ? "Updating…" : "Update available");
  print_fit(c, W / 2, 92, c.f.t26, c.ink, TextAlign::TOP_CENTER, "Sticky Remote " + m.update_version, W - 2 * M - 20);
  if (!m.fw_version.empty())
    print_fit(c, W / 2, 128, c.f.t20, c.ink, TextAlign::TOP_CENTER, "This one runs " + m.fw_version, W - 2 * M - 20);
  const int tx = M + 24, tw = W - 2 * M - 48;
  if (busy) {
    c.it.print(W / 2, 290, c.f.i120, c.ink, TextAlign::CENTER, ICON_DOWNLOAD);
    print_wrapped(c, tx, 380, c.f.t20, c.ink,
                  "Downloading from GitHub and installing. Leave it be: the screen stays like this until it "
                  "restarts by itself, in about a minute.", tw, 28);
    return;
  }
  // What's new: one item per line, as many as fit above the note.
  const int note_y = m.update_step == Model::Upd::FAILED ? 476 : 504;
  int y = 186;
  if (!m.update_notes.empty()) {
    c.it.print(tx, y, c.f.t20, c.ink, TextAlign::TOP_LEFT, "What's new");
    y += 32;
    size_t start = 0;
    bool cut = false;
    while (start < m.update_notes.size()) {
      size_t end = m.update_notes.find('\n', start);
      if (end == std::string::npos) end = m.update_notes.size();
      std::string item = m.update_notes.substr(start, end - start);
      start = end + 1;
      while (!item.empty() && (item[0] == '-' || item[0] == ' ' || item[0] == '*')) item.erase(0, 1);
      if (item.empty()) continue;
      // Measure first: an item that would run into the note is left out.
      int lines = 1, w = 0;
      for (size_t i = 0, sp = 0; i <= item.size(); i++)
        if (i == item.size() || item[i] == ' ') {
          const int ww = text_width(c, c.f.t18, item.substr(sp, i - sp).c_str()) + 6;
          if (w + ww > tw - 18 && w > 0) { lines++; w = 0; }
          w += ww;
          sp = i + 1;
        }
      if (y + lines * 23 > note_y - 36) { cut = true; break; }   // a line kept for "and more"
      c.it.filled_circle(tx + 4, y + 11, 3, c.ink);
      y = print_wrapped(c, tx + 18, y, c.f.t18, c.ink, item, tw - 18, 23) + 5;
    }
    if (cut) c.it.print(tx + 18, y, c.f.t18, c.ink, TextAlign::TOP_LEFT, "… and more on GitHub");
  }
  print_wrapped(c, tx, note_y, c.f.t18, c.ink,
                m.update_step == Model::Upd::FAILED ? "The last try failed (download or check). Nothing changed."
                                                    : "About a minute. Wi-Fi, settings and pairing are kept.",
                tw, 23);
  const int bx = M + 30, bw = W - 2 * M - 60;
  if (update_can_install(m)) {
    fill_rrect(c, bx, UPD_INSTALL_Y, bw, UPD_BTN_H, 18, c.ink);
    c.it.print(W / 2, UPD_INSTALL_Y + UPD_BTN_H / 2, c.f.t26, c.paper, TextAlign::CENTER,
               m.update_step == Model::Upd::FAILED ? "Try again" : "Install");
  } else {
    print_wrapped(c, bx, UPD_INSTALL_Y + 6, c.f.t20, c.ink, "Battery low: plug it in to update.", bw, 28);
  }
  stroke_rrect(c, bx, UPD_CLOSE_Y, bw, UPD_BTN_H, 18, 3);
  c.it.print(W / 2, UPD_CLOSE_Y + UPD_BTN_H / 2, c.f.t26, c.ink, TextAlign::CENTER, "Not now");
}

inline void draw_screen(Ctx &c, const Model &m, const GrayImage &poster,
                        int64_t now_epoch, bool time_valid, int hour, int minute, const char *room,
                        const char *name1, const char *name2, const char *infuse_label,
                        const char *player = "Apple TV") {
  c.it.fill(c.paper);
  if (m.sleeping && m.in_setup()) return draw_setup_sleep(c, m);
  if (m.sleeping) return draw_asleep(c, m, room, time_valid, hour, minute);
  switch (m.setup()) {
    case Setup::WIFI: return draw_setup_wifi(c, m);
    case Setup::CONFIGURE: return draw_setup_configure(c, m);
    case Setup::HOME_ASSISTANT: return draw_setup_ha(c, m);
    case Setup::CALIBRATE: return draw_calibrate(c);
    case Setup::NONE: break;
  }
  if (m.update_prompt) return draw_update(c, m);
  draw_status(c, m, room, time_valid, hour, minute);
  draw_tiles(c, m, name1, name2, player);
  const Mode mode = m.mode();
  if (mode == Mode::OFF) {
    draw_off(c);
  } else {
    draw_remote(c, m, now_epoch);
    draw_now(c, m, poster, now_epoch);
    draw_volume(c, m);
  }
}

struct Labels {
  const char *room, *light_1, *light_2, *infuse;
  const char *player = "Apple TV";   // the power tile's label
};

// The display lambda: draws the screen and records what it showed, so the
// minute tick knows when the clock or seek bar needs a redraw.
inline void render(Ctx &c, const GrayImage &poster, int64_t now_epoch, bool time_valid,
                   int hour, int minute, const Labels &l) {
  const auto &m = model();
  draw_screen(c, m, poster, now_epoch, time_valid, hour, minute, l.room, l.light_1, l.light_2, l.infuse,
              l.player);
  auto &r = redraw();
  r.drawn_clock = time_valid ? hour * 60 + minute : -2;
  r.drawn_minute = m.mode() == Mode::PLAYING ? shown_minute(m, now_epoch) : -1;
}

// Every few seconds: redraw only when the clock or seek-bar minute changed.
inline void minute_tick(bool time_valid, int hour, int minute, int64_t now_epoch) {
  if (!time_valid) return;
  const auto &m = model();
  const auto &r = redraw();
  const int clock = hour * 60 + minute;
  const int shown = m.mode() == Mode::PLAYING ? shown_minute(m, now_epoch) : -1;
  if (clock != r.drawn_clock || shown != r.drawn_minute) request();
}

// --- Hit testing ----------------------------------------------------------------
enum Action {
  NONE = 0,
  LIGHT_1,
  LIGHT_2,
  ATV_POWER,
  PLAY_PAUSE,
  SKIP_BACK,
  SKIP_FWD,
  SEEK,        // seek_to holds the target in seconds
  VOL_DOWN,
  VOL_UP,
  OPEN_INFUSE,
  TURN_ON,
  TAB_NOW,
  TAB_REMOTE,
  NAV_UP,
  NAV_DOWN,
  NAV_LEFT,
  NAV_RIGHT,
  NAV_SELECT,
  NAV_BACK,
  NAV_HOME,
  UPDATE_OPEN,      // the status bar, when an update is available
  UPDATE_INSTALL,
  UPDATE_CLOSE,
};

// remote.send_command name for each pad action (Apple TV integration).
inline const char *nav_command(Action a) {
  switch (a) {
    case NAV_UP: return "up";
    case NAV_DOWN: return "down";
    case NAV_LEFT: return "left";
    case NAV_RIGHT: return "right";
    case NAV_SELECT: return "select";
    case NAV_BACK: return "menu";
    case NAV_HOME: return "home";
    default: return nullptr;
  }
}

inline bool in(int x, int y, int x0, int y0, int w, int h) {
  return x >= x0 && x < x0 + w && y >= y0 && y < y0 + h;
}

inline Action hit(const Model &m, int x, int y, int64_t now_epoch, float &seek_to) {
  if (m.in_setup()) return NONE;
  if (m.update_prompt) {
    if (m.update_step == Model::Upd::INSTALLING) return NONE;
    if (update_can_install(m) && in(x, y, M + 30, UPD_INSTALL_Y - 10, W - 2 * M - 60, UPD_BTN_H + 20)) return UPDATE_INSTALL;
    if (in(x, y, M + 30, UPD_CLOSE_Y - 10, W - 2 * M - 60, UPD_BTN_H + 20)) return UPDATE_CLOSE;
    return NONE;
  }
  if (!m.update_version.empty() && y < TILE_Y - 6) return UPDATE_OPEN;
  const int n = tile_count(m.light_count);
  for (int i = 0; i < n; i++)
    if (in(x, y, tile_x(i, n) - TILE_GAP / 2, TILE_Y - 6, tile_w(n) + TILE_GAP, TILE_H + 10))   // down to the tabs
      return i == n - 1 ? ATV_POWER : (Action) (LIGHT_1 + i);

  const Mode mode = m.mode();
  if (mode != Mode::OFF && in(x, y, 0, VOL_Y - 8, W, VOL_H + 16)) {
    if (x < M + VOL_BTN_W + 20) return VOL_DOWN;
    if (x > W - M - VOL_BTN_W - 20) return VOL_UP;
    return NONE;
  }
  if (mode == Mode::OFF) return y > TILE_Y + TILE_H + 12 ? TURN_ON : NONE;   // the whole lower half

  // Pad: the centre is OK, the ring splits into four 90-degree wedges.
  const int dx = x - PAD_CX, dy = y - PAD_CY;
  const int d2 = dx * dx + dy * dy;
  if (d2 <= PAD_R_IN * PAD_R_IN) return NAV_SELECT;
  if (d2 <= (PAD_R + 8) * (PAD_R + 8)) {
    if (std::abs(dx) > std::abs(dy)) return dx < 0 ? NAV_LEFT : NAV_RIGHT;
    return dy < 0 ? NAV_UP : NAV_DOWN;
  }
  if (in(x, y, 0, KEY_Y - 2, W, KEY_H + 8)) {
    const int i = std::max(0, std::min(3, (x - M + KEY_GAP / 2) / (KEY_W + KEY_GAP)));
    static const Action keys[4] = {NAV_BACK, NAV_HOME, OPEN_INFUSE, PLAY_PAUSE};
    return keys[i];
  }
  if (mode != Mode::PLAYING || !in(x, y, 0, NP_Y - 2, W, NP_H + 6)) return NONE;

  // What's playing. The artwork is a large play/pause target.
  if (x < NP_X - 7) return PLAY_PAUSE;
  if (y >= SKIP_Y - 6) return x < skip_x(1) - SKIP_GAP / 2 ? SKIP_BACK : SKIP_FWD;
  if (y >= SEEK_Y - 12) {   // a generous band around the thin bar
    if (std::isnan(m.duration) || m.duration <= 0) return NONE;
    float f = (float) (x - SEEK_X0) / (float) (SEEK_X1 - SEEK_X0);
    f = std::max(0.0f, std::min(1.0f, f));
    seek_to = f * m.duration;
    return SEEK;
  }
  return NONE;
}

// New Apple TV state from HA.
inline void set_atv_state(const std::string &state) {
  auto &m = model();
  const Mode before = m.mode();
  m.atv_state = state;
  const Mode after = m.mode();
  if (after != Mode::PLAYING && before == Mode::PLAYING) clear_media(m, true);
  m.state_seen = true;
  request();
}

// --- Optimistic updates ----------------------------------------------------------
// E-paper is slow, so show the expected result at once; HA's state update
// confirms (or corrects) it a moment later.
inline void optimistic_toggle_light(int i) {
  auto &l = model().lights[i];
  if (!l.known()) return;
  l.state = l.on() ? "off" : "on";
  request();
}

inline void optimistic_play_pause(int64_t now_epoch) {
  auto &m = model();
  const float p = live_position(m, now_epoch);
  if (!std::isnan(p)) {
    m.position = p;
    m.position_at = now_epoch;
  }
  if (m.atv_state == "playing") m.atv_state = "paused";
  else if (m.atv_state == "paused") m.atv_state = "playing";
  request();
}

inline void optimistic_seek(float to, int64_t now_epoch) {
  auto &m = model();
  m.position = to;
  m.position_at = now_epoch;
  request();
}

// --- Values from Home Assistant and the device's settings -----------------------
inline float to_float(const std::string &v) {
  if (v.empty() || v == "None" || v == "unknown" || v == "unavailable" || v == "nan") return NAN;
  char *end = nullptr;
  const float f = strtof(v.c_str(), &end);
  return end == v.c_str() ? NAN : f;
}

/// One value, by key: what the firmware's HA subscriptions and settings feed in,
/// and what the simulator's sc_set passes on. Keys: atv_state title series artist
/// app season episode position duration volume position_at (ISO 8601) picture
/// light0_state light1_state light0_brightness light1_brightness online usb
/// battery sleeping net ip ha_paired device_name setup_url
/// light_count light_icon0 light_icon1 configured app_button.
inline void apply(const std::string &key, const std::string &v) {
  auto &m = model();
  if (key == "atv_state") return set_atv_state(v);
  if (key == "title") m.title = clean(v);
  else if (key == "series") m.series = clean(v);
  else if (key == "artist") m.artist = clean(v);
  else if (key == "app") {
    const std::string app = clean(v);
    if (app != m.app) clear_media(m, false);   // the new app's details follow (subscribed after the app)
    m.app = app;
  }
  else if (key == "season") m.season = to_float(v);
  else if (key == "episode") m.episode = to_float(v);
  else if (key == "position") m.position = to_float(v);
  else if (key == "duration") m.duration = to_float(v);
  else if (key == "volume") m.volume = to_float(v);
  else if (key == "position_at") m.position_at = parse_iso8601(v);
  else if (key == "light0_state") m.lights[0].state = v;
  else if (key == "light1_state") m.lights[1].state = v;
  else if (key == "light0_brightness") m.lights[0].brightness = to_float(v);
  else if (key == "light1_brightness") m.lights[1].brightness = to_float(v);
  else if (key == "online") m.online = v == "1";
  else if (key == "usb") m.usb = v == "1";
  else if (key == "sleeping") {
    m.sleeping = v == "1";
    return request(true);
  } else if (key == "battery") {
    const float x = to_float(v);
    if (std::isnan(m.battery) || (int) m.battery != (int) x) request();
    m.battery = x;
    return;
  } else if (key == "net") {   // connected, connecting, no_network, unreachable
    return set_network(v == "connected" ? Net::CONNECTED : v == "no_network" || v == "hotspot" ? Net::NO_NETWORK
                       : v == "unreachable" ? Net::UNREACHABLE : Net::CONNECTING, m.ip);
  } else if (key == "ip") {
    return set_network(m.net, v);
  } else if (key == "ha_paired") {
    return set_ha_paired(v == "1");
  } else if (key == "device_name") m.device_name = v;
  else if (key == "setup_url") m.setup_url = v;
  else if (key == "light_count") {
    m.light_count = v == "1" ? 1 : 2;
    return request(true);
  } else if (key == "light_icon0" || key == "light_icon1") {
    m.light_icon[key.back() - '0'] = light_icon_index(v);
  } else if (key == "app_button") {
    m.app_button = v;
  } else if (key == "configured") {
    m.configured = v == "1";
    return request(true);
  } else if (key == "fw_version") {
    m.fw_version = v;
    return;
  } else if (key == "update_version") {   // empty: none available
    if (v == m.update_version) return;
    m.update_version = v;
    if (v.empty()) m.update_prompt = false;
  } else if (key == "update_notes") {
    const std::string notes = clean(v);
    if (notes == m.update_notes) return;
    m.update_notes = notes;
    if (!m.update_prompt) return;
  } else if (key == "update_failed") {
    m.update_step = Model::Upd::FAILED;
    m.update_prompt = true;
    return request(true);
  } else if (key == "sleep_clock") {
    m.sleep_clock = v == "1";
    if (!m.sleeping) return;
  } else if (key == "date") {   // "day-of-week day month", as ESPTime counts them (Sunday = 1)
    int dow = 0, day = 0, month = 0;
    sscanf(v.c_str(), "%d %d %d", &dow, &day, &month);
    if (dow == m.dow && day == m.day && month == m.month) return;
    m.dow = dow; m.day = day; m.month = month;
    if (!m.sleeping) return;
  } else if (key == "picture") {
    // New artwork: no poster until its download lands.
    const std::string pic = clean(v);
    if (pic == m.picture) return;
    m.picture = pic;
    m.poster_loaded = false;
    return request();
  } else {
    return;
  }
  request();
}

// --- Input handling ----------------------------------------------------------------
// What a tap or button press asks Home Assistant to do. The firmware sends
// these as homeassistant.action calls; the simulator fakes HA with them.
struct Actions {
  virtual ~Actions() = default;
  virtual void click() = 0;                       // audible tick
  virtual void toggle_light(int i) = 0;
  virtual void atv_power(bool on) = 0;
  virtual void play_pause() = 0;
  virtual void seek(float seconds) = 0;
  virtual void remote_command(const char *command) = 0;
  virtual void volume(bool up) = 0;
  virtual void open_infuse() = 0;
  virtual void save_touch_cal(const TouchCal &cal) {}   // the firmware stores it
  virtual void sleep() {}                               // the AI button: deep sleep now
  virtual void install_update() {}                      // download and flash the newer release
};

inline Actions *&actions() {
  static Actions *a = nullptr;
  return a;
}

// --- Hold to repeat ------------------------------------------------------------
// Keeping a finger on an arrow (or a volume end) repeats it, so long lists
// scroll without tapping. A repeat sends the command and nothing else: the
// arrow is already shown pressed, so the screen has nothing to redraw.
// HOLD_MAX_MS bounds a release the touch controller never reported: after it
// the finger has to lift and press again, which also ends a runaway repeat.
constexpr uint32_t HOLD_START_MS = 450, HOLD_EVERY_MS = 220, HOLD_MAX_MS = 3000;

struct Hold {
  Action action = NONE;
  uint32_t since = 0, last = 0;
};

inline Hold &hold() {
  static Hold h;
  return h;
}

inline bool repeats(Action a) {
  return a == NAV_UP || a == NAV_DOWN || a == NAV_LEFT || a == NAV_RIGHT || a == VOL_UP || a == VOL_DOWN;
}

/// The finger left the screen.
inline void touch_released() { hold().action = NONE; }

/// Call often while a finger may be down (the firmware: every 20 ms).
inline void hold_tick(Actions &act) {
  auto &h = hold();
  if (h.action == NONE) return;
  const uint32_t now = esphome::millis();
  if (now - h.since > HOLD_MAX_MS || model().sleeping) {   // a release that never came
    h.action = NONE;
    return;
  }
  if (now - h.since < HOLD_START_MS || now - h.last < HOLD_EVERY_MS) return;
  h.last = now;
  touched();
  if (h.action == VOL_UP || h.action == VOL_DOWN) {
    act.volume(h.action == VOL_UP);
  } else {
    act.remote_command(nav_command(h.action));
    model().pressed_at = now;   // the arrow stays shown as pressed
  }
}

inline Action handle_tap(int x, int y, int64_t now_epoch, Actions &act) {
  auto &m = model();
  if (m.sleeping) return NONE;
  float seek_to = 0;
  const Action a = hit(m, x, y, now_epoch, seek_to);
  hold().action = NONE;
  if (a == NONE) return a;
  touched();
  if (a == UPDATE_OPEN || a == UPDATE_CLOSE || a == UPDATE_INSTALL) {
    act.click();
    if (a == UPDATE_INSTALL) {
      m.update_step = Model::Upd::INSTALLING;
      act.install_update();
    } else {
      m.update_prompt = a == UPDATE_OPEN;
      if (m.update_step == Model::Upd::FAILED && a == UPDATE_CLOSE) m.update_step = Model::Upd::IDLE;
    }
    request(true);
    return a;
  }
  if (repeats(a)) hold() = Hold{a, esphome::millis(), esphome::millis()};
  act.click();
  if (m.mode() != Mode::OFF) {
    switch (a) {
      case NAV_UP: press_feedback(Press::UP); break;
      case NAV_DOWN: press_feedback(Press::DOWN); break;
      case NAV_LEFT: press_feedback(Press::LEFT); break;
      case NAV_RIGHT: press_feedback(Press::RIGHT); break;
      case NAV_SELECT: press_feedback(Press::OK); break;
      case NAV_BACK: press_feedback(Press::KEY_BACK); break;
      case NAV_HOME: press_feedback(Press::KEY_HOME); break;
      case OPEN_INFUSE: press_feedback(Press::KEY_INFUSE); break;
      case PLAY_PAUSE: press_feedback(Press::KEY_PLAY); break;
      case SKIP_BACK: press_feedback(Press::KEY_REW); break;
      case SKIP_FWD: press_feedback(Press::KEY_FF); break;
      default: break;
    }
  }
  const float pos = live_position(m, now_epoch);
  switch (a) {
    case LIGHT_1:
    case LIGHT_2: {
      const int i = a == LIGHT_1 ? 0 : 1;
      optimistic_toggle_light(i);
      act.toggle_light(i);
      break;
    }
    case ATV_POWER: act.atv_power(!m.atv_on()); break;
    case TURN_ON: act.atv_power(true); break;
    case OPEN_INFUSE: act.open_infuse(); break;
    case PLAY_PAUSE:
      optimistic_play_pause(now_epoch);
      act.play_pause();
      break;
    case SEEK:
      optimistic_seek(seek_to, now_epoch);
      act.seek(seek_to);
      break;
    case SKIP_BACK:
    case SKIP_FWD:
      if (std::isnan(pos)) {
        act.remote_command(a == SKIP_BACK ? "skip_backward" : "skip_forward");
      } else {
        const float to = a == SKIP_BACK ? std::max(0.0f, pos - SKIP_SECONDS) : pos + SKIP_SECONDS;
        optimistic_seek(to, now_epoch);
        act.seek(to);
      }
      break;
    case VOL_DOWN: act.volume(false); break;
    case VOL_UP: act.volume(true); break;
    default:
      if (const char *cmd = nav_command(a)) act.remote_command(cmd);
      break;
  }
  return a;
}

/// A touch from the chip: raw coordinates (rx, ry) and ESPHome's transformed
/// ones (x, y). Calibration, when saved, replaces ESPHome's transform.
inline Action handle_touch(int rx, int ry, int x, int y, int64_t now_epoch, Actions &act) {
  if (model().calibrating) {
    act.click();
    calib_tap(rx, ry);
    return NONE;
  }
  if (touch_cal().valid) map_touch(touch_cal(), rx, ry, x, y);
  return handle_tap(x, y, now_epoch, act);
}

// Physical buttons: AI = sleep now (any button wakes), Up/Down = volume.
// Play/Pause is always on screen, so the AI button doesn't double as it.
// While calibrating: AI saves, Up starts again, Down cancels.
enum Button { BTN_AI, BTN_UP, BTN_DOWN };

inline void handle_button(Button b, int64_t now_epoch, Actions &act) {
  auto &m = model();
  touched();
  if (m.calibrating && !m.sleeping) {
    if (b == BTN_UP) return calib_start();
    if (b == BTN_DOWN) return calib_stop();
    if (calib().step == CAL_N && calib().fit.valid) {
      touch_cal() = calib().fit;
      act.save_touch_cal(touch_cal());
      calib_stop();
    }
    return;
  }
  if (m.sleeping) return;
  if (b == BTN_AI) return act.sleep();   // during setup too
  if (m.in_setup()) return;
  switch (b) {
    case BTN_AI: break;
    case BTN_UP: act.volume(true); break;
    case BTN_DOWN: act.volume(false); break;
  }
}

// --- Power latch and deep sleep -------------------------------------------------
// GPIO45 (PWR_HOLD) and GPIO46 (PWR_LOCK) keep the battery rail latched and
// GPIO39 (charger enable, active low) must stay low so USB can charge during
// sleep. Their levels are frozen across deep sleep and released on boot, after
// the YAML has re-driven the same levels.
inline void hold_pins_for_sleep() {
  gpio_hold_en(GPIO_NUM_45);
  gpio_hold_en(GPIO_NUM_46);
  gpio_hold_en(GPIO_NUM_39);
  gpio_deep_sleep_hold_en();
  // Keep pull-ups on the three active-low buttons so the ext1 ANY_LOW mask
  // never watches a floating line (needs the RTC peripheral domain on).
  for (auto pin : {GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6}) {
    rtc_gpio_pullup_en(pin);
    rtc_gpio_pulldown_dis(pin);
  }
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
}

inline void release_sleep_holds() {
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis(GPIO_NUM_45);
  gpio_hold_dis(GPIO_NUM_46);
  gpio_hold_dis(GPIO_NUM_39);
}

}  // namespace remote_ui
