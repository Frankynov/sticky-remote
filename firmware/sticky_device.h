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
//   {"sticky":"setup","cmd":"scan"}                    -> starts a Wi-Fi scan; "get" lists the networks
//   {"sticky":"setup","cmd":"calibrate"}               -> shows the touch calibration screen (dev builds)
//   {"sticky":"setup","cmd":"touch_reset"}             -> forgets the touch calibration (dev builds)
// The board also sends the info line ("event":"hello") once at boot. Send at
// most 64 bytes every ~10 ms: the receive buffer is small.

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>

#include <JPEGDEC.h>

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
#include "driver/gpio.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "remote_ui.h"

namespace sticky {

static const char *const TAG = "sticky";
constexpr int PROTOCOL = 1;
// What this firmware's USB protocol can do beyond version 1, for the setup page.
static const char *const CAPS[] = {"scan", "touch_sound", "sleep_after", "sleep_clock", "unpair"};

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

// --- Touch sound (its own preference: the settings' stored layout stays as it was) ---
static const char *const SOUND_NAMES[] = {"off", "quiet", "normal"};

inline uint8_t &sound() {
  static uint8_t level = 1;   // quiet
  return level;
}

inline esphome::ESPPreferenceObject &sound_pref() {
  static esphome::ESPPreferenceObject p = esphome::global_preferences->make_preference<uint8_t>(0x53534E44, true);
  return p;
}

inline void load_sound() {
  uint8_t v = 1;
  if (sound_pref().load(&v) && v <= 2) sound() = v;
}

inline void set_sound(uint8_t level) {
  if (level > 2 || level == sound()) return;
  sound() = level;
  sound_pref().save(&level);
  esphome::global_preferences->sync();
}

// --- Sleep on battery (its own preference too) -------------------------------------
// Minutes without a tap or a button press before deep sleep; "never" keeps it
// awake until the AI button is pressed. On USB power it never sleeps by itself.
static const char *const SLEEP_NAMES[] = {"1", "3", "10", "30", "never"};
constexpr uint16_t SLEEP_MINUTES[] = {1, 3, 10, 30, 0};
constexpr uint8_t SLEEP_CHOICES = 5;

inline uint8_t &sleep_choice() {
  static uint8_t choice = 1;   // 3 minutes
  return choice;
}

inline esphome::ESPPreferenceObject &sleep_pref() {
  static esphome::ESPPreferenceObject p = esphome::global_preferences->make_preference<uint8_t>(0x53534C50, true);
  return p;
}

inline void load_sleep() {
  uint8_t v = 1;
  if (sleep_pref().load(&v) && v < SLEEP_CHOICES) sleep_choice() = v;
}

inline void set_sleep(uint8_t choice) {
  if (choice >= SLEEP_CHOICES || choice == sleep_choice()) return;
  sleep_choice() = choice;
  sleep_pref().save(&choice);
  esphome::global_preferences->sync();
}

/// Milliseconds of inactivity before sleeping on battery; 0 = never.
inline uint32_t sleep_after_ms() { return (uint32_t) SLEEP_MINUTES[sleep_choice()] * 60000u; }

// --- Clock while asleep (its own preference) --------------------------------------
// On: asleep on battery, the panel shows the time and the firmware dozes in
// light sleep, waking once a minute to move it (doze() below). Off: the dark
// "Asleep" face and deep sleep, which uses less.
inline bool &clock_on() {
  static bool on = true;
  return on;
}

inline esphome::ESPPreferenceObject &clock_pref() {
  static esphome::ESPPreferenceObject p = esphome::global_preferences->make_preference<uint8_t>(0x53434C4B, true);
  return p;
}

inline void load_clock() {
  uint8_t v = 1;
  if (clock_pref().load(&v)) clock_on() = v != 0;
  remote_ui::apply("sleep_clock", clock_on() ? "1" : "0");
}

inline void set_clock(bool on) {
  if (on == clock_on()) return;
  clock_on() = on;
  const uint8_t v = on ? 1 : 0;
  clock_pref().save(&v);
  esphome::global_preferences->sync();
  remote_ui::apply("sleep_clock", on ? "1" : "0");
}

enum class Woke { TIMER, BUTTON, USB, OTHER, REJECTED };

/// Light sleep until the next whole minute (at most `max_s` seconds), a button
/// (low on GPIO4/5/6) or USB power (high on GPIO9). Wi-Fi must be off first.
/// Everything is kept: the panel's comparison frame, the clock, the settings.
inline Woke doze(uint32_t max_s) {
  const time_t t = ::time(nullptr);
  uint32_t s = max_s;
  if (t > 1600000000) s = std::min<uint32_t>(max_s, (uint32_t) (60 - t % 60));
  if (s == 0) s = 60;
  // The latch, charger and touch supply keep their levels; the buttons keep their pull-ups.
  for (auto pin : {GPIO_NUM_45, GPIO_NUM_46, GPIO_NUM_39, GPIO_NUM_42, GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_9})
    gpio_sleep_sel_dis(pin);
  for (auto pin : {GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6}) gpio_wakeup_enable(pin, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable(GPIO_NUM_9, GPIO_INTR_HIGH_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup((uint64_t) s * 1000000ULL + 200000ULL);   // just past the minute mark
  const esp_err_t err = esp_light_sleep_start();
  const auto cause = esp_sleep_get_wakeup_cause();
  if (err != ESP_OK) {
    // Refused (a wake source already active, or the chip's state): say why,
    // wait a little so the loop doesn't spin, and let the caller try again.
    ESP_LOGW("sticky", "light sleep refused: %s", esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(2000));
  } else {
    ESP_LOGD("sticky", "dozed %u s, woke by %s", (unsigned) s,
             cause == ESP_SLEEP_WAKEUP_TIMER ? "the timer" : cause == ESP_SLEEP_WAKEUP_GPIO ? "a pin" : "something else");
  }
  // Leave nothing armed: deep sleep (clock off) sets up its own wake sources.
  for (auto pin : {GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_9}) gpio_wakeup_disable(pin);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  esphome::App.feed_wdt();
  if (err != ESP_OK) return Woke::REJECTED;
  if (cause == ESP_SLEEP_WAKEUP_TIMER) return Woke::TIMER;
  if (cause == ESP_SLEEP_WAKEUP_GPIO) return gpio_get_level(GPIO_NUM_9) ? Woke::USB : Woke::BUTTON;
  return Woke::OTHER;
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
  // With the calibration tool off (released firmware), only the built-in map counts.
  if (remote_ui::calibration_enabled() && cal_pref().load(&st) && st.magic == CAL_MAGIC) {
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
  remote_ui::apply("sleep_clock", clock_on() ? "1" : "0");
}

// --- Artwork ------------------------------------------------------------------------
// Downloaded and decoded in a task of its own, so the main loop (touch, beeps,
// the display) never waits on DNS, a TLS handshake or a JPEG decode: on the main
// loop those froze the Sticky for seconds. JPEGs are decoded at 1/2, 1/4 or 1/8
// scale straight to about the size they're drawn at. Anything else (PNG) falls
// back to ESPHome's online_image, which still blocks the loop while it loads.
inline esphome::online_image::OnlineImage *&poster() {
  static esphome::online_image::OnlineImage *p = nullptr;
  return p;
}

constexpr size_t MAX_ARTWORK = 3 * 1024 * 1024;

struct PosterLoader {
  enum Result : uint8_t { NONE, READY, NOT_JPEG, FAILED };
  std::mutex mu;                 // guards the request and result fields
  TaskHandle_t task = nullptr;
  std::string want_url;          // request
  uint32_t want_gen = 0;
  uint8_t *res_px = nullptr;     // result, handed to the main loop
  int res_w = 0, res_h = 0;
  uint32_t res_gen = 0;
  Result res = NONE;
  // Main loop only: what's shown.
  uint8_t *px = nullptr;
  int w = 0, h = 0;
  std::string url;               // the current artwork's URL
  bool fallback = false;         // online_image holds the current artwork
};

inline PosterLoader &loader() {
  static PosterLoader l;
  return l;
}

struct JpegOut {
  uint8_t *px;
  int w, h;
};

inline int jpeg_draw(JPEGDRAW *d) {
  auto *o = static_cast<JpegOut *>(d->pUser);
  const auto *src = reinterpret_cast<const uint8_t *>(d->pPixels);
  const int cols = std::min(d->iWidthUsed, o->w - d->x);
  for (int y = 0; y < d->iHeight && cols > 0; y++) {
    const int oy = d->y + y;
    if (oy >= o->h) break;
    memcpy(o->px + (size_t) oy * o->w + d->x, src + (size_t) y * d->iWidth, cols);
  }
  return 1;
}

/// Runs in the loader task.
inline PosterLoader::Result load_artwork(const std::string &url, uint8_t *&out, int &ow, int &oh) {
  using R = PosterLoader::Result;
  esp_http_client_config_t cfg{};
  cfg.url = url.c_str();
  cfg.timeout_ms = 10000;
  cfg.disable_auto_redirect = true;   // the proxy URL carries its own token
  cfg.buffer_size = 2048;
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif
  esp_http_client_handle_t http = esp_http_client_init(&cfg);
  if (http == nullptr) return R::FAILED;
  const uint32_t t0 = esphome::millis();
  uint8_t *data = nullptr;
  size_t n = 0, cap = 0;
  bool ok = false;
  if (esp_http_client_open(http, 0) == ESP_OK) {
    const int64_t len = esp_http_client_fetch_headers(http);
    const int status = esp_http_client_get_status_code(http);
    if (status != 200) {
      ESP_LOGW(TAG, "artwork: HTTP %d", status);
    } else if (len <= (int64_t) MAX_ARTWORK) {
      cap = len > 0 ? (size_t) len : 64 * 1024;
      data = static_cast<uint8_t *>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      while (data != nullptr) {
        if (n == cap) {
          if (cap >= MAX_ARTWORK) break;
          cap = std::min(cap * 2, MAX_ARTWORK);
          auto *bigger = static_cast<uint8_t *>(heap_caps_realloc(data, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
          if (bigger == nullptr) break;
          data = bigger;
        }
        const int got = esp_http_client_read(http, reinterpret_cast<char *>(data) + n, cap - n);
        if (got < 0) break;
        if (got == 0) {
          ok = len <= 0 || n == (size_t) len;
          break;
        }
        n += got;
      }
    }
  }
  esp_http_client_cleanup(http);
  R r = R::FAILED;
  if (ok && n >= 4 && !(data[0] == 0xFF && data[1] == 0xD8)) {
    r = R::NOT_JPEG;
  } else if (ok && n >= 4) {
    auto *jpeg = new JPEGDEC();   // ~20 KB of tables and buffers
    if (jpeg->openRAM(data, (int) n, jpeg_draw) && jpeg->getJPEGType() != JPEG_MODE_PROGRESSIVE) {
      const int sw = jpeg->getWidth(), sh = jpeg->getHeight();
      // The largest JPEG scale that still leaves at least the size it's drawn at.
      const float fit = std::min((float) remote_ui::POSTER_W / sw, (float) remote_ui::POSTER_H / sh);
      int scale = 1;
      for (int s : {8, 4, 2}) {
        if (fit * s <= 1.0f) {
          scale = s;
          break;
        }
      }
      JpegOut o{nullptr, (sw + scale - 1) / scale, (sh + scale - 1) / scale};
      o.px = static_cast<uint8_t *>(heap_caps_calloc((size_t) o.w * o.h, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (o.px != nullptr) {
        jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
        jpeg->setUserPointer(&o);
        const int opt = scale == 8 ? JPEG_SCALE_EIGHTH : scale == 4 ? JPEG_SCALE_QUARTER : scale == 2 ? JPEG_SCALE_HALF : 0;
        if (jpeg->decode(0, 0, opt)) {
          out = o.px;
          ow = o.w;
          oh = o.h;
          r = R::READY;
          ESP_LOGI(TAG, "artwork: %dx%d JPEG, %u bytes, decoded at 1/%d to %dx%d in %u ms", sw, sh, (unsigned) n,
                   scale, o.w, o.h, (unsigned) (esphome::millis() - t0));
        } else {
          free(o.px);
        }
      }
      jpeg->close();
    } else {
      ESP_LOGW(TAG, "artwork: not a JPEG it can decode (progressive?)");
    }
    delete jpeg;
  } else {
    ESP_LOGW(TAG, "artwork: download failed after %u ms (%u bytes)", (unsigned) (esphome::millis() - t0), (unsigned) n);
  }
  free(data);
  return r;
}

inline void poster_task(void *) {
  auto &L = loader();
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    std::string url;
    uint32_t gen;
    {
      std::lock_guard<std::mutex> g(L.mu);
      url = L.want_url;
      gen = L.want_gen;
    }
    if (url.empty()) continue;
    uint8_t *px = nullptr;
    int w = 0, h = 0;
    const auto r = load_artwork(url, px, w, h);
    std::lock_guard<std::mutex> g(L.mu);
    if (gen != L.want_gen) {   // superseded: the newer request is already notified
      free(px);
      continue;
    }
    free(L.res_px);
    L.res_px = px;
    L.res_w = w;
    L.res_h = h;
    L.res_gen = gen;
    L.res = r;
  }
}

/// Starts loading `picture` (HA's entity_picture); an empty one cancels.
inline void fetch_poster(const std::string &picture) {
  auto &L = loader();
  std::string url = picture;
  if (!picture.empty() && picture.rfind("http", 0) != 0) {
    std::string base = settings().ha_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    url = base.empty() ? std::string() : base + picture;
  }
  L.url = url;
  L.fallback = false;
  {
    std::lock_guard<std::mutex> g(L.mu);
    L.want_url = url;
    L.want_gen++;
  }
  if (url.empty()) return;
  if (L.task == nullptr)
    xTaskCreatePinnedToCore(poster_task, "artwork", 12 * 1024, nullptr, 1, &L.task, 0);
  if (L.task != nullptr) xTaskNotifyGive(L.task);
}

/// Main loop: takes a finished download.
inline void poster_poll() {
  auto &L = loader();
  PosterLoader::Result r;
  uint8_t *px;
  int w, h;
  {
    std::lock_guard<std::mutex> g(L.mu);
    if (L.res == PosterLoader::NONE) return;
    r = L.res;
    px = L.res_px;
    w = L.res_w;
    h = L.res_h;
    const bool current = L.res_gen == L.want_gen;
    L.res = PosterLoader::NONE;
    L.res_px = nullptr;
    if (!current) {
      free(px);
      return;
    }
  }
  auto &m = remote_ui::model();
  if (r == PosterLoader::READY && !m.picture.empty()) {
    free(L.px);
    L.px = px;
    L.w = w;
    L.h = h;
    m.poster_loaded = true;
    remote_ui::request_big();   // partial now; cleaned once the Sticky is left alone
    return;
  }
  free(px);
  if (r == PosterLoader::NOT_JPEG && poster() != nullptr && !L.url.empty()) {
    L.fallback = true;   // online_image decodes other formats
    poster()->set_url(L.url);
    poster()->update();
    return;
  }
  m.poster_loaded = false;
  remote_ui::request();
}

/// online_image finished (or failed) a fallback download.
inline void fallback_done(bool ok) {
  if (!loader().fallback) return;
  remote_ui::model().poster_loaded = ok && !remote_ui::model().picture.empty();
  if (ok) remote_ui::request_big(); else remote_ui::request();
}

/// The artwork to draw.
inline remote_ui::GrayImage poster_image() {
  auto &L = loader();
  auto *img = poster();
  if (L.fallback && img != nullptr && img->is_loaded())
    return {remote_ui::gray_buffer(img), img->get_buffer_width(), img->get_buffer_height()};
  if (!L.fallback && L.px != nullptr) return {L.px, L.w, L.h};
  return {};
}

/// Keeps the Wi-Fi scan results in memory (ESPHome drops them once connected),
/// so the setup page can list networks whenever it asks.
inline void keep_scan_results() {
  if (esphome::wifi::global_wifi_component != nullptr)
    esphome::wifi::global_wifi_component->set_keep_scan_results(true);
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
  // HA forwards in subscription order: the state and the app come first, since
  // they clear the details (remote_ui::clear_media) that follow them.
  sub(s.player, nullptr, "atv_state");
  sub(s.player, "app_name", "app");
  sub(s.player, "media_title", "title");
  sub(s.player, "media_series_title", "series");
  sub(s.player, "media_artist", "artist");
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
    st["touch_sound"] = SOUND_NAMES[sound()];
    st["sleep_after"] = SLEEP_NAMES[sleep_choice()];
    st["sleep_clock"] = clock_on() ? "on" : "off";
    JsonArray caps = root["caps"].to<JsonArray>();
    for (const char *cap : CAPS) caps.add(cap);
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

// --- The Wi-Fi network sent over USB --------------------------------------------
// ESPHome keeps the network it was given at runtime under a key that depends
// on the build (with a network in the config) or a fixed one (without): the
// placeholder network in wifi-setup.yaml moved it, and 0.6.0/0.6.1 forgot the
// network 0.5.x had saved. A copy under a key of our own survives any build;
// restore_wifi() hands it to ESPHome before Wi-Fi starts, taking the old
// fixed key's network over as it goes.
inline esphome::ESPPreferenceObject &wifi_copy_pref() {
  static esphome::ESPPreferenceObject p =
      esphome::global_preferences->make_preference<esphome::wifi::SavedWifiSettings>(0x53574946, true);
  return p;
}

inline void save_wifi(const std::string &ssid, const std::string &pass) {
  esphome::wifi::global_wifi_component->save_wifi_sta(ssid, pass);
  esphome::wifi::SavedWifiSettings s{};
  snprintf(s.ssid, sizeof s.ssid, "%s", ssid.c_str());
  snprintf(s.password, sizeof s.password, "%s", pass.c_str());
  wifi_copy_pref().save(&s);
  esphome::global_preferences->sync();
}

/// Before Wi-Fi starts: the network saved over USB replaces the configured one.
inline void restore_wifi() {
  esphome::wifi::SavedWifiSettings s{};
  bool have = wifi_copy_pref().load(&s) && s.ssid[0] != '\0';
  if (!have) {   // saved by 0.5.x, under ESPHome's key for a config without networks
    auto legacy = esphome::global_preferences->make_preference<esphome::wifi::SavedWifiSettings>(88491487UL, true);
    have = legacy.load(&s) && s.ssid[0] != '\0';
    if (have) wifi_copy_pref().save(&s);
  }
  if (!have) return;
  s.ssid[sizeof s.ssid - 1] = '\0';
  s.password[sizeof s.password - 1] = '\0';
  esphome::wifi::WiFiAP ap;
  ap.set_ssid(s.ssid);
  ap.set_password(s.password);
  esphome::wifi::global_wifi_component->set_sta(ap);
  ESP_LOGI("sticky", "Wi-Fi: using the saved network %s", s.ssid);
}

/// Set by the YAML: forgets that Home Assistant was ever connected.
inline std::function<void()> &unpair_hook() {
  static std::function<void()> f;
  return f;
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
      save_wifi(ssid, pass);
    }
    if (!in.isNull() && in["touch_sound"].is<const char *>()) {
      const std::string level = in["touch_sound"].as<const char *>();
      for (uint8_t i = 0; i < 3; i++)
        if (level == SOUND_NAMES[i]) set_sound(i);
    }
    if (!in.isNull() && in["sleep_after"].is<const char *>()) {
      const std::string choice = in["sleep_after"].as<const char *>();
      for (uint8_t i = 0; i < SLEEP_CHOICES; i++)
        if (choice == SLEEP_NAMES[i]) set_sleep(i);
    }
    if (!in.isNull() && in["sleep_clock"].is<const char *>()) {
      const std::string v = in["sleep_clock"].as<const char *>();
      if (v == "on" || v == "off") set_clock(v == "on");
    }
    if (!in.isNull() && !save(s)) return send_error("Could not save the settings.");
    send_line(esphome::json::build_json([](JsonObject root) {
      root["sticky"] = "setup";
      root["ok"] = true;
      root["saved"] = true;
      root["restarting"] = true;
    }));
    restart_at() = esphome::millis() + 800;
  } else if (cmd == "unpair") {           // forget the key Home Assistant gave it, so HA can add it afresh
#if defined(USE_API_NOISE) && !defined(USE_API_NOISE_PSK_FROM_YAML)
    esphome::api::global_api_server->clear_noise_psk(true);
#endif
    if (unpair_hook()) unpair_hook()();
    remote_ui::set_ha_paired(false);
    send_line(esphome::json::build_json([](JsonObject root) {
      root["sticky"] = "setup";
      root["ok"] = true;
      root["unpaired"] = true;
    }));
  } else if (cmd == "scan") {             // looks for Wi-Fi networks; ask again in ~3 s for the list
    // Straight to the driver: ESPHome's own scan belongs to its connection
    // logic. Its handler stores the results (kept, see keep_scan_results()).
    wifi_scan_config_t cfg{};
    cfg.show_hidden = false;
    const esp_err_t err = esp_wifi_scan_start(&cfg, false);
    send_line(esphome::json::build_json([err](JsonObject root) {
      root["sticky"] = "setup";
      root["ok"] = true;
      root["scanning"] = err == ESP_OK;   // false: one is already running, or it's connecting
    }));
  } else if (cmd == "calibrate") {        // shows the touch calibration screen (dev builds)
    if (!remote_ui::calibration_enabled()) return send_error("Touch calibration isn't in this firmware.");
    remote_ui::calib_start();
    send_line(info("info"));
  } else if (cmd == "touch_reset") {      // forgets the calibration (dev builds)
    if (!remote_ui::calibration_enabled()) return send_error("Touch calibration isn't in this firmware.");
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
  poster_poll();
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
