// Sticky core: the firmware's remote_ui.h behind a plain C interface, so the
// macOS app (Swift) and the command-line simulator drive exactly the code that
// runs on the device. One engine per process (remote_ui keeps global state).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { SC_WIDTH = 480, SC_HEIGHT = 800 };
enum { SC_FONT_COUNT = 10 };  // t18 t20 t22 t26 t30 i24 i40 i52 i64 i120, in that order
enum { SC_REFRESH_NONE = 0, SC_REFRESH_PARTIAL = 1, SC_REFRESH_FULL = 2 };
enum { SC_BUTTON_AI = 0, SC_BUTTON_UP = 1, SC_BUTTON_DOWN = 2 };
enum { SC_MODE_OFF = 0, SC_MODE_IDLE = 1, SC_MODE_PLAYING = 2 };
enum { SC_PAGE_NOW = 0, SC_PAGE_REMOTE = 1 };

/// Name of font `index` as used in the YAML ("t18", …, "i120").
const char *sc_font_name(int index);
/// Starts (or replaces) font `index`. ascent = ESPHome's baseline, height = line height.
void sc_font_begin(int index, int ascent, int height);
/// Adds a glyph. `bits` are packed MSB-first, row after row, as ESPHome stores them.
void sc_font_glyph(int index, uint32_t codepoint, int advance, int offset_x, int offset_y, int width,
                   int height, const uint8_t *bits);
/// Loads a font from a sim/build/fonts/*.fnt file. Returns 0 on success.
int sc_font_load_fnt(int index, const char *path);
/// Same, from the file's text already in memory (the web page has no files).
int sc_font_load_fnt_text(int index, const char *text);

void sc_set_labels(const char *room, const char *light_1, const char *light_2, const char *infuse,
                   const char *player);
/// Feeds one Home Assistant value, as the firmware's on_value handlers do. Keys:
/// atv_state title series artist app season episode position duration volume
/// position_at (ISO 8601) picture light0_state light1_state light0_brightness
/// light1_brightness online usb battery sleeping. Use "None" for absent.
void sc_set(const char *key, const char *value);
/// Artwork downloaded (8-bit grayscale, row-major). NULL clears it.
void sc_set_poster(const uint8_t *gray, int width, int height);

/// Receives each Home Assistant call the firmware makes, e.g. "toggle_light 0",
/// "atv_power on", "play_pause", "seek 120", "remote select", "volume up",
/// "open_infuse", and "beep" for the click sound.
typedef void (*sc_call_fn)(void *context, const char *call);
/// A tap at panel coordinates. Returns the action (see sc_action_name).
int sc_tap(int x, int y, int64_t now_epoch, sc_call_fn fn, void *context);
void sc_button(int button, int64_t now_epoch, sc_call_fn fn, void *context);
const char *sc_action_name(int action);

/// The firmware's 5-second tick: requests a redraw when the clock or seek-bar minute changed.
void sc_tick(int time_valid, int hour, int minute, int64_t now_epoch);
/// The refresh scheduler: when a refresh is due, renders it into `pixels`
/// (SC_WIDTH*SC_HEIGHT bytes, 1 = paper, 0 = ink) and returns its kind.
int sc_frame(int time_valid, int hour, int minute, int64_t now_epoch, uint8_t *pixels);
/// Forces the next frame to be a full refresh (e.g. after fonts changed).
void sc_request_full(void);

int sc_mode(void);
int sc_page(void);
int sc_sleeping(void);
/// A setup screen (hotspot or "add to Home Assistant") is showing.
int sc_in_setup(void);

#ifdef __cplusplus
}
#endif
