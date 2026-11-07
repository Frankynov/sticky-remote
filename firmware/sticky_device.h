#pragma once
// Sticky Remote, device side: settings stored on the Sticky, Home Assistant
// subscriptions made at boot from them, and the USB setup protocol the web
// wizard (and Sticky Simulator) use. Firmware only; the drawing and the
// handling of values live in remote_ui.h, shared with the simulator.
//
// Settings: what the Sticky controls (player, its remote, the app button, one
// or two lights, labels, icons), the HA address for artwork and the typeface.
// Their defaults come from the YAML substitutions; once a setup tool has sent
// settings over USB, the stored ones win.
//
// USB setup protocol (UART0, 115200, shared with the log): newline-delimited
// JSON, one object per line. The host keeps only lines starting with '{' that
// carry "sticky":"setup". Commands:
//   {"sticky":"setup","cmd":"get"}                     -> info line
//   {"sticky":"setup","cmd":"set","settings":{...},"wifi":{"ssid":"…","pass":"…"}}
//                                                      -> {"ok":true,"saved":true,"restarting":true}
//   {"sticky":"setup","cmd":"reset"}                   -> clears the settings, restarts
//   {"sticky":"setup","cmd":"calibrate"}               -> shows the touch calibration screen
//   {"sticky":"setup","cmd":"touch_reset"}             -> forgets the touch calibration
// The board also sends the info line ("event":"hello") once at boot. Send at
// most 64 bytes every ~10 ms: the receive buffer is small.

#include <cstring>
#include <string>

#include "esphome/components/api/api_server.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/logger/logger.h"
#include "esphome/components/online_image/online_image.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"

#include "driver/uart.h"
#include "remote_ui.h"

namespace sticky {

static const char *const TAG = "sticky";
constexpr int PROTOCOL = 1;

struct Settings {
  std::string player, remote, player_name, app, app_label;
  std::string light1, light1_name, light1_icon, light2, light2_name, light2_icon;
  std::string room, ha_url, font_set;  // font_set: "inter" or "trmnl"
  bool configured() const { return !player.empty() || !light1.empty(); }
  int light_count() const { return light2.empty() ? 1 : 2; }
};

// The stored form: fixed-size fields (preferences store plain bytes).
struct Stored {
  uint32_t magic;
  char player[80], remote[80], player_name[32], app[48], app_label[40];
  char light1[80], light1_name[32], light1_icon[12], light2[80], light2_name[32], light2_icon[12];
  char room[32], ha_url[120], font_set[8];
};
constexpr uint32_t STORED_MAGIC = 0x53544B31;  // "STK1"

inline Settings &settings() {
  static Settings s;
  return s;
}
inline Settings &defaults() {
  static Settings s;
  return s;
}
inline bool &stored_present() {
  static bool b = false;
  return b;
}

inline esphome::ESPPreferenceObject &pref() {
  static esphome::ESPPreferenceObject p = esphome::global_preferences->make_preference<Stored>(0x5354494B, true);
  return p;
}

#define STICKY_FIELDS(X) \
  X(player) X(remote) X(player_name) X(app) X(app_label) X(light1) X(light1_name) X(light1_icon) \
  X(light2) X(light2_name) X(light2_icon) X(room) X(ha_url) X(font_set)

inline void copy_out(char *dst, size_t n, const std::string &src) {
  strncpy(dst, src.c_str(), n - 1);
  dst[n - 1] = 0;
}

/// Loads stored settings (falling back to the YAML defaults).
inline void load() {
  Stored st{};
  stored_present() = pref().load(&st) && st.magic == STORED_MAGIC;
  auto &s = settings();
  s = defaults();
  if (stored_present()) {
#define X(f) s.f = st.f;
    STICKY_FIELDS(X)
#undef X
  }
  ESP_LOGI(TAG, "settings: %s, player=%s light1=%s light2=%s", stored_present() ? "stored" : "defaults",
           s.player.c_str(), s.light1.c_str(), s.light2.c_str());
}

inline bool save(const Settings &s) {
  Stored st{};
  st.magic = STORED_MAGIC;
#define X(f) copy_out(st.f, sizeof st.f, s.f);
  STICKY_FIELDS(X)
#undef X
  const bool ok = pref().save(&st) && esphome::global_preferences->sync();
  if (ok) stored_present() = true;
  return ok;
}

// --- Touch calibration (its own preference, so settings resets keep it) -------
struct StoredCal {
  uint32_t magic;
  float v[6];   // ax bx cx ay by cy, see remote_ui::TouchCal
};
constexpr uint32_t CAL_MAGIC = 0x54434131;  // "TCA1"

inline esphome::ESPPreferenceObject &cal_pref() {
  static esphome::ESPPreferenceObject p = esphome::global_preferences->make_preference<StoredCal>(0x53544341, true);
  return p;
}

/// The map used when none is saved on the device (the touch_map substitution).
inline remote_ui::TouchCal &default_touch_cal() {
  static remote_ui::TouchCal c;
  return c;
}

inline void set_default_touch_cal(const char *map) {
  auto &c = default_touch_cal();
  c = remote_ui::TouchCal{};
  c.valid = map != nullptr && sscanf(map, "%f,%f,%f,%f,%f,%f", &c.ax, &c.bx, &c.cx, &c.ay, &c.by, &c.cy) == 6;
}

inline bool &touch_cal_stored() {
  static bool b = false;
  return b;
}

inline void load_touch_cal() {
  StoredCal st{};
  auto &c = remote_ui::touch_cal();
  c = default_touch_cal();
  touch_cal_stored() = false;
  if (cal_pref().load(&st) && st.magic == CAL_MAGIC) {
    touch_cal_stored() = true;
    c.valid = true;
    c.ax = st.v[0]; c.bx = st.v[1]; c.cx = st.v[2];
    c.ay = st.v[3]; c.by = st.v[4]; c.cy = st.v[5];
  }
  ESP_LOGI(TAG, "touch calibration: %s", touch_cal_stored() ? "stored" : c.valid ? "default map" : "none (YAML transform)");
}

/// Saves a calibration; an invalid one clears it (back to the default map).
inline bool save_touch_cal(const remote_ui::TouchCal &c) {
  StoredCal st{};
  if (c.valid) {
    st.magic = CAL_MAGIC;
    const float v[6] = {c.ax, c.bx, c.cx, c.ay, c.by, c.cy};
    memcpy(st.v, v, sizeof v);
  }
  remote_ui::touch_cal() = c.valid ? c : default_touch_cal();
  touch_cal_stored() = c.valid;
  return cal_pref().save(&st) && esphome::global_preferences->sync();
}

inline void forget() {
  Stored st{};
  pref().save(&st);
  esphome::global_preferences->sync();
  stored_present() = false;
}

// --- Labels and look -----------------------------------------------------------
inline const remote_ui::Labels &labels() {
  static remote_ui::Labels l{"", "", "", "", ""};
  const auto &s = settings();
  l.room = s.room.c_str();
  l.light_1 = s.light1_name.c_str();
  l.light_2 = s.light2_name.c_str();
  l.infuse = s.app_label.c_str();
  l.player = s.player_name.empty() ? "Player" : s.player_name.c_str();
  return l;
}

inline bool trmnl() { return settings().font_set == "trmnl"; }

/// Pushes the settings that change the layout into the shared UI model.
inline void apply_to_ui() {
  const auto &s = settings();
  remote_ui::apply("light_count", s.light_count() == 1 ? "1" : "2");
  remote_ui::apply("light_icon0", s.light1_icon);
  remote_ui::apply("light_icon1", s.light2_icon);
  remote_ui::apply("configured", s.configured() ? "1" : "0");
  remote_ui::apply("app_button", s.app);
}

// --- Home Assistant subscriptions ------------------------------------------------
inline esphome::online_image::OnlineImage *&poster() {
  static esphome::online_image::OnlineImage *p = nullptr;
  return p;
}

inline void fetch_poster(const std::string &picture) {
  auto *img = poster();
  if (img == nullptr || picture.empty()) return;
  std::string url = picture;
  if (picture.rfind("http", 0) != 0) {
    std::string base = settings().ha_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.empty()) return;
    url = base + picture;
  }
  img->set_url(url);
  img->update();
}

/// Subscribes to the chosen entities. Call before the API connects (on_boot).
inline void subscribe(esphome::api::APIServer *api) {
  if (api == nullptr) return;
  const auto &s = settings();
  auto sub = [api](const std::string &entity, const char *attribute, const char *key) {
    if (entity.empty()) return;
    esphome::optional<std::string> attr;
    if (attribute != nullptr) attr = std::string(attribute);
    const std::string k = key;
    api->subscribe_home_assistant_state(entity, attr, [k](esphome::StringRef v) {
      const std::string value(v.c_str(), v.size());
      if (k == "picture") {
        const std::string before = remote_ui::model().picture;
        remote_ui::apply(k, value);
        const std::string after = remote_ui::model().picture;
        if (after != before) fetch_poster(after);
        return;
      }
      remote_ui::apply(k, value);
    });
  };
  sub(s.player, nullptr, "atv_state");
  sub(s.player, "media_title", "title");
  sub(s.player, "media_series_title", "series");
  sub(s.player, "media_artist", "artist");
  sub(s.player, "app_name", "app");
  sub(s.player, "media_season", "season");
  sub(s.player, "media_episode", "episode");
  sub(s.player, "media_position", "position");
  sub(s.player, "media_duration", "duration");
  sub(s.player, "volume_level", "volume");
  sub(s.player, "media_position_updated_at", "position_at");
  sub(s.player, "entity_picture", "picture");
  sub(s.light1, nullptr, "light0_state");
  sub(s.light1, "brightness", "light0_brightness");
  sub(s.light2, nullptr, "light1_state");
  sub(s.light2, "brightness", "light1_brightness");
}

// --- USB setup protocol ------------------------------------------------------------
inline uart_port_t uart_num() { return esphome::logger::global_logger->get_uart_num(); }

inline void send_line(const std::string &json) {
  const std::string line = "\n" + json + "\n";   // own line, even mid-log
  uart_write_bytes(uart_num(), line.data(), line.size());
}

inline std::string info(const char *event) {
  return esphome::json::build_json([event](JsonObject root) {
    root["sticky"] = "setup";
    root["event"] = event;
    root["ok"] = true;
    root["protocol"] = PROTOCOL;
#ifdef ESPHOME_PROJECT_VERSION
    root["fw"] = ESPHOME_PROJECT_VERSION;
#endif
    root["name"] = esphome::App.get_name().c_str();
    char mac[18];
    root["mac"] = esphome::get_mac_address_pretty_into_buffer(mac);
    auto *w = esphome::wifi::global_wifi_component;
    const auto &m = remote_ui::model();
    JsonObject wifi = root["wifi"].to<JsonObject>();
    wifi["connected"] = w != nullptr && w->is_connected();
    wifi["hotspot"] = w != nullptr && w->is_ap_active();
    wifi["ip"] = m.ip;
    if (w != nullptr && w->is_connected()) {
      char ssid[esphome::wifi::SSID_BUFFER_SIZE];
      wifi["ssid"] = w->wifi_ssid_to(ssid);
    }
    JsonObject ha = root["ha"].to<JsonObject>();
    ha["connected"] = m.online;
    ha["paired"] = m.ha_paired;
    root["configured"] = settings().configured();
    {
      const auto &tc = remote_ui::touch_cal();
      JsonObject touch = root["touch"].to<JsonObject>();
      touch["calibrated"] = touch_cal_stored();
      touch["source"] = touch_cal_stored() ? "stored" : tc.valid ? "default" : "yaml";
      touch["calibrating"] = m.calibrating;
      if (tc.valid) {
        bool swap, mx, my;
        remote_ui::describe_cal(tc, swap, mx, my);
        touch["swap_xy"] = swap;
        touch["mirror_x"] = mx;
        touch["mirror_y"] = my;
        JsonArray map = touch["map"].to<JsonArray>();
        for (float v : {tc.ax, tc.bx, tc.cx, tc.ay, tc.by, tc.cy}) map.add(v);
      }
    }
    root["stored"] = stored_present();
    JsonObject st = root["settings"].to<JsonObject>();
#define X(f) st[#f] = settings().f;
    STICKY_FIELDS(X)
#undef X
    if (w != nullptr) {
      JsonArray nets = root["networks"].to<JsonArray>();
      int n = 0;
      for (const auto &r : w->get_scan_result()) {
        if (n++ >= 20) break;
        JsonObject o = nets.add<JsonObject>();
        o["ssid"] = r.get_ssid().c_str();
        o["rssi"] = r.get_rssi();
        o["secure"] = r.get_with_auth();
      }
    }
  });
}

inline void send_error(const std::string &message) {
  send_line(esphome::json::build_json([&](JsonObject root) {
    root["sticky"] = "setup";
    root["ok"] = false;
    root["error"] = message;
  }));
}

inline uint32_t &restart_at() {
  static uint32_t t = 0;
  return t;
}

inline void handle(const std::string &line) {
  JsonDocument doc = esphome::json::parse_json(line);
  if (doc.isNull() || doc["sticky"] != "setup") return;
  const std::string cmd = doc["cmd"] | "";
  if (cmd == "get") {
    send_line(info("info"));
  } else if (cmd == "set") {
    Settings s = settings();
    JsonObject in = doc["settings"];
    if (!in.isNull()) {
#define X(f) if (in[#f].is<const char *>()) s.f = in[#f].as<const char *>();
      STICKY_FIELDS(X)
#undef X
    }
    JsonObject wifi = doc["wifi"];
    if (!wifi.isNull() && wifi["ssid"].is<const char *>()) {
      const std::string ssid = wifi["ssid"].as<const char *>();
      const std::string pass = wifi["pass"] | "";
      if (ssid.empty() || ssid.size() > 32 || pass.size() > 64) return send_error("Wi-Fi name or password too long.");
      esphome::wifi::global_wifi_component->save_wifi_sta(ssid, pass);
    }
    if (!in.isNull() && !save(s)) return send_error("Could not save the settings.");
    send_line(esphome::json::build_json([](JsonObject root) {
      root["sticky"] = "setup";
      root["ok"] = true;
      root["saved"] = true;
      root["restarting"] = true;
    }));
    restart_at() = esphome::millis() + 800;
  } else if (cmd == "calibrate") {        // shows the touch calibration screen
    remote_ui::calib_start();
    send_line(info("info"));
  } else if (cmd == "touch_reset") {      // forgets the calibration
    save_touch_cal(remote_ui::TouchCal{});
    send_line(info("info"));
  } else if (cmd == "reset") {
    forget();
    send_line(esphome::json::build_json([](JsonObject root) {
      root["sticky"] = "setup";
      root["ok"] = true;
      root["reset"] = true;
      root["restarting"] = true;
    }));
    restart_at() = esphome::millis() + 800;
  } else {
    send_error("Unknown command: " + cmd);
  }
}

/// Reads the serial line; call often (every loop). Restarts after a `set`.
inline void poll() {
  static std::string buf;
  uint8_t chunk[128];
  for (;;) {
    const int n = uart_read_bytes(uart_num(), chunk, sizeof chunk, 0);
    if (n <= 0) break;
    for (int i = 0; i < n; i++) {
      const char ch = (char) chunk[i];
      if (ch == '\n' || ch == '\r') {
        if (!buf.empty() && buf[0] == '{') handle(buf);
        buf.clear();
      } else if (buf.size() < 2048) {
        buf += ch;
      }
    }
  }
  if (restart_at() != 0 && (int32_t) (esphome::millis() - restart_at()) >= 0) {
    restart_at() = 0;
    esphome::App.safe_reboot();
  }
}

}  // namespace sticky
