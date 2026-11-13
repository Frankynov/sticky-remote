// Sticky core: see include/sticky_core.h.
#include "sticky_core.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "remote_ui.h"
#include "esphome/components/qr_code/qr_code.h"

using namespace esphome;

namespace {

display::BaseFont g_fonts[SC_FONT_COUNT];
const char *const kFontNames[SC_FONT_COUNT] = {"t18", "t20", "t22", "t26", "t30",
                                               "i24", "i40", "i52", "i64", "i120", "clock"};

const remote_ui::Fonts &fonts() {
  static const remote_ui::Fonts f{&g_fonts[0], &g_fonts[1], &g_fonts[2], &g_fonts[3], &g_fonts[4],
                                  &g_fonts[5], &g_fonts[6], &g_fonts[7], &g_fonts[8], &g_fonts[9], &g_fonts[10]};
  return f;
}

struct Labels {
  std::string room = "Living room", light_1 = "Floor lamp", light_2 = "Ceiling", infuse = "Open Infuse", player = "Apple TV";
  remote_ui::Labels view() const {
    return {room.c_str(), light_1.c_str(), light_2.c_str(), infuse.c_str(), player.c_str()};
  }
} g_labels;

// The artwork, as online_image (type GRAYSCALE) decodes it.
struct Poster : runtime_image::RuntimeImage {
  std::vector<uint8_t> data;
  void set(const uint8_t *gray, int w, int h) {
    data.assign(gray, gray + (size_t) w * h);
    buffer_ = data.data();
    buffer_width_ = w;
    buffer_height_ = h;
  }
  void clear() {
    data.clear();
    buffer_ = nullptr;
    buffer_width_ = buffer_height_ = 0;
  }
} g_poster;

display::Display g_display;

// The setup screen's QR code (the firmware's is a qr_code component).
qr_code::QrCode &qr() {
  static qr_code::QrCode q;
  static bool linked = (remote_ui::wifi_qr() = &q, true);
  (void) linked;
  return q;
}

// Forwards remote_ui's Actions to the host's callback.
struct CallbackActions : remote_ui::Actions {
  sc_call_fn fn = nullptr;
  void *ctx = nullptr;
  void send(const std::string &s) {
    if (fn) fn(ctx, s.c_str());
  }
  void click() override { send("beep"); }
  void toggle_light(int i) override { send("toggle_light " + std::to_string(i)); }
  void atv_power(bool on) override { send(on ? "atv_power on" : "atv_power off"); }
  void play_pause() override { send("play_pause"); }
  void seek(float s) override { send("seek " + std::to_string((int) s)); }
  void remote_command(const char *c) override { send(std::string("remote ") + c); }
  void volume(bool up) override { send(up ? "volume up" : "volume down"); }
  void open_infuse() override { send("open_infuse"); }
  void sleep() override { send("sleep"); }
} g_actions;

}  // namespace

extern "C" {

const char *sc_font_name(int index) { return index >= 0 && index < SC_FONT_COUNT ? kFontNames[index] : ""; }

void sc_font_begin(int index, int ascent, int height) {
  if (index < 0 || index >= SC_FONT_COUNT) return;
  auto &f = g_fonts[index];
  f.glyphs.clear();
  f.ascent = ascent;
  f.descent = height - ascent;
  f.first_advance = 0;
}

void sc_font_glyph(int index, uint32_t cp, int advance, int ox, int oy, int w, int h, const uint8_t *bits) {
  if (index < 0 || index >= SC_FONT_COUNT) return;
  auto &f = g_fonts[index];
  display::Glyph g;
  g.advance = advance;
  g.ox = ox;
  g.oy = oy;
  g.w = w;
  g.h = h;
  g.bits.assign((size_t) w * h, 0);
  for (int i = 0; i < w * h && bits != nullptr; i++) g.bits[i] = (bits[i / 8] >> (7 - i % 8)) & 1;
  if (f.glyphs.empty() || cp < f.glyphs.begin()->first) f.first_advance = advance;
  f.glyphs[cp] = std::move(g);
}

int sc_font_load_fnt(int index, const char *path) {
  std::ifstream in(path);
  if (!in) return -1;
  std::stringstream text;
  text << in.rdbuf();
  return sc_font_load_fnt_text(index, text.str().c_str());
}

int sc_font_load_fnt_text(int index, const char *text_c) {
  std::istringstream in(text_c ? text_c : "");
  std::string line;
  std::vector<uint8_t> bytes;
  while (std::getline(in, line)) {
    std::istringstream ss(line);
    std::string tag;
    ss >> tag;
    if (tag == "font") {
      int ascent, height;
      ss >> ascent >> height;
      sc_font_begin(index, ascent, height);
    } else if (tag == "g") {
      uint32_t cp;
      int adv, ox, oy, w, h;
      std::string hex;
      ss >> cp >> adv >> ox >> oy >> w >> h >> hex;
      bytes.clear();
      if (hex != "-")
        for (size_t i = 0; i + 2 <= hex.size(); i += 2)
          bytes.push_back((uint8_t) std::stoi(hex.substr(i, 2), nullptr, 16));
      sc_font_glyph(index, cp, adv, ox, oy, w, h, bytes.data());
    }
  }
  return 0;
}

void sc_set_labels(const char *room, const char *l1, const char *l2, const char *infuse, const char *player) {
  g_labels.player = player;
  g_labels.room = room;
  g_labels.light_1 = l1;
  g_labels.light_2 = l2;
  g_labels.infuse = infuse;
  remote_ui::request(true);
}

// Same effect as the firmware's Home Assistant subscriptions (remote_ui::apply).
void sc_set(const char *key_c, const char *v_c) {
  const std::string key = key_c, v = v_c ? v_c : "";
  remote_ui::apply(key, v);
  if (key == "ap_ssid" || key == "ap_password" || key == "device_name") {
    auto &m = remote_ui::model();
    qr().set_value(remote_ui::wifi_qr_text(m.ap_ssid, m.ap_password));
  }
}

void sc_set_poster(const uint8_t *gray, int w, int h) {
  if (gray == nullptr || w < 2 || h < 2) {
    g_poster.clear();
    remote_ui::model().poster_loaded = false;
  } else {
    g_poster.set(gray, w, h);
    remote_ui::model().poster_loaded = true;  // on_download_finished
  }
  remote_ui::request_big();
}

int sc_tap(int x, int y, int64_t now, sc_call_fn fn, void *ctx) {
  g_actions.fn = fn;
  g_actions.ctx = ctx;
  return (int) remote_ui::handle_tap(x, y, now, g_actions);
}

int sc_touch(int rx, int ry, int64_t now, sc_call_fn fn, void *ctx) {
  g_actions.fn = fn;
  g_actions.ctx = ctx;
  return (int) remote_ui::handle_touch(rx, ry, rx, ry, now, g_actions);
}

void sc_release(void) { remote_ui::touch_released(); }

void sc_hold_tick(sc_call_fn fn, void *ctx) {
  g_actions.fn = fn;
  g_actions.ctx = ctx;
  remote_ui::hold_tick(g_actions);
}

void sc_calibrate(void) {
  remote_ui::calibration_enabled() = true;   // the engine is a development tool
  remote_ui::calib_start();
}

int sc_touch_cal(float out[6]) {
  const auto &c = remote_ui::touch_cal();
  const float v[6] = {c.ax, c.bx, c.cx, c.ay, c.by, c.cy};
  for (int i = 0; i < 6; i++) out[i] = v[i];
  return c.valid ? 1 : 0;
}

void sc_button(int button, int64_t now, sc_call_fn fn, void *ctx) {
  g_actions.fn = fn;
  g_actions.ctx = ctx;
  remote_ui::handle_button((remote_ui::Button) button, now, g_actions);
}

const char *sc_action_name(int a) {
  static const char *const names[] = {"NONE", "LIGHT_1", "LIGHT_2", "ATV_POWER", "PLAY_PAUSE", "SKIP_BACK",
                                      "SKIP_FWD", "SEEK", "VOL_DOWN", "VOL_UP", "OPEN_INFUSE", "TURN_ON",
                                      "TAB_NOW", "TAB_REMOTE", "NAV_UP", "NAV_DOWN", "NAV_LEFT", "NAV_RIGHT",
                                      "NAV_SELECT", "NAV_BACK", "NAV_HOME"};
  return a >= 0 && a < (int) (sizeof names / sizeof *names) ? names[a] : "?";
}

void sc_tick(int valid, int hour, int minute, int64_t now) { remote_ui::minute_tick(valid, hour, minute, now); }

int sc_frame(int valid, int hour, int minute, int64_t now, uint8_t *pixels) {
  remote_ui::feedback_tick();
  bool full = false;
  if (!remote_ui::take_refresh(remote_ui::model(), full)) return SC_REFRESH_NONE;
  remote_ui::Ctx c{g_display, fonts()};
  remote_ui::GrayImage art;
  if (!g_poster.data.empty()) art = {g_poster.data.data(), g_poster.get_buffer_width(), g_poster.get_buffer_height()};
  remote_ui::render(c, art, now, valid, hour, minute, g_labels.view());
  if (pixels != nullptr) memcpy(pixels, g_display.px.data(), g_display.px.size());
  return full ? SC_REFRESH_FULL : SC_REFRESH_PARTIAL;
}

void sc_request_full(void) { remote_ui::request(true); }

int sc_mode(void) {
  switch (remote_ui::model().mode()) {
    case remote_ui::Mode::PLAYING: return SC_MODE_PLAYING;
    case remote_ui::Mode::IDLE: return SC_MODE_IDLE;
    default: return SC_MODE_OFF;
  }
}

int sc_page(void) { return remote_ui::model().page == remote_ui::Page::NOW ? SC_PAGE_NOW : SC_PAGE_REMOTE; }
int sc_sleeping(void) { return remote_ui::model().sleeping ? 1 : 0; }
int sc_in_setup(void) { return remote_ui::model().in_setup() ? 1 : 0; }

}  // extern "C"
