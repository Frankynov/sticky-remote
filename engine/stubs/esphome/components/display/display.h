#pragma once
// Simulator stand-in for esphome::display::Display: a 1-bit framebuffer with
// the handful of primitives remote_ui.h draws with. Fonts are glyph bitmaps
// rasterised by sim/make_fonts.py (Pillow, no anti-aliasing), like ESPHome's
// font component does at build time.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "esphome/core/color.h"

namespace esphome {
namespace display {

static const Color COLOR_OFF(0, 0, 0, 0);
static const Color COLOR_ON(255, 255, 255, 255);

enum class TextAlign {
  TOP = 0x00, CENTER_VERTICAL = 0x01, BASELINE = 0x02, BOTTOM = 0x04,
  LEFT = 0x00, CENTER_HORIZONTAL = 0x08, RIGHT = 0x10,
  TOP_LEFT = TOP | LEFT, TOP_CENTER = TOP | CENTER_HORIZONTAL, TOP_RIGHT = TOP | RIGHT,
  CENTER_LEFT = CENTER_VERTICAL | LEFT, CENTER = CENTER_VERTICAL | CENTER_HORIZONTAL,
  CENTER_RIGHT = CENTER_VERTICAL | RIGHT,
  BASELINE_LEFT = BASELINE | LEFT, BASELINE_CENTER = BASELINE | CENTER_HORIZONTAL,
  BASELINE_RIGHT = BASELINE | RIGHT,
  BOTTOM_LEFT = BOTTOM | LEFT, BOTTOM_CENTER = BOTTOM | CENTER_HORIZONTAL, BOTTOM_RIGHT = BOTTOM | RIGHT,
};

struct Glyph {
  int advance = 0, ox = 0, oy = 0, w = 0, h = 0;  // oy: from the top of the line
  std::vector<uint8_t> bits;                      // w*h, 1 = ink of the glyph
};

class BaseFont {
 public:
  int ascent = 0, descent = 0;
  std::map<uint32_t, Glyph> glyphs;
  int height() const { return ascent + descent; }
  int first_advance = 0;  // ESPHome: unknown characters advance by the first glyph's width
  int fallback_advance() const { return first_advance; }
  const Glyph *find(uint32_t cp) const {
    auto it = glyphs.find(cp);
    return it == glyphs.end() ? nullptr : &it->second;
  }
};

inline uint32_t next_cp(const char *&s) {
  const uint8_t c = (uint8_t) *s++;
  if (c < 0x80) return c;
  int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
  uint32_t cp = c & (0x3F >> extra);
  while (extra-- && (*s & 0xC0) == 0x80) cp = (cp << 6) | ((uint8_t) *s++ & 0x3F);
  return cp;
}

class Display {
 public:
  static constexpr int WIDTH = 480, HEIGHT = 800;
  std::vector<uint8_t> px = std::vector<uint8_t>(WIDTH * HEIGHT, 1);  // 1 = paper (white)

  virtual ~Display() = default;
  void draw_pixel_at(int x, int y, Color c) {
    if (x < 0 || y < 0 || x >= WIDTH || y >= HEIGHT) return;
    px[y * WIDTH + x] = c.is_on() ? 1 : 0;
  }
  void fill(Color c) { std::fill(px.begin(), px.end(), c.is_on() ? 1 : 0); }
  void filled_rectangle(int x, int y, int w, int h, Color c) {
    for (int j = y; j < y + h; j++)
      for (int i = x; i < x + w; i++) draw_pixel_at(i, j, c);
  }
  void rectangle(int x, int y, int w, int h, Color c) {
    filled_rectangle(x, y, w, 1, c);
    filled_rectangle(x, y + h - 1, w, 1, c);
    filled_rectangle(x, y, 1, h, c);
    filled_rectangle(x + w - 1, y, 1, h, c);
  }
  // Same midpoint fill as ESPHome's Display::filled_circle.
  void filled_circle(int cx, int cy, int r, Color c) {
    int dx = -r, dy = 0, err = 2 - 2 * r, e2;
    do {
      for (int hl = cx + dx; hl <= cx - dx; hl++) {
        draw_pixel_at(hl, cy + dy, c);
        draw_pixel_at(hl, cy - dy, c);
      }
      e2 = err;
      if (e2 < dy) {
        err += ++dy * 2 + 1;
        if (-dx == dy && e2 <= dx) e2 = 0;
      }
      if (e2 > dx) err += ++dx * 2 + 1;
    } while (dx <= 0);
  }

  // Font::measure, as ESPHome does it: width excludes the first glyph's
  // left offset; an unknown character advances by the first glyph's width.
  void measure(const char *text, BaseFont *f, int *width, int *x_offset) {
    int min_x = 0, x = 0;
    bool has_char = false;
    for (const char *s = text; *s;) {
      const Glyph *g = f->find(next_cp(s));
      if (g == nullptr) {
        x += f->fallback_advance();
        continue;
      }
      min_x = has_char ? std::min(min_x, x + g->ox) : g->ox;
      x += g->advance;
      has_char = true;
    }
    *x_offset = min_x;
    *width = x - min_x;
  }

  // Display::get_text_bounds, as ESPHome does it.
  void get_text_bounds(int x, int y, const char *text, BaseFont *f, TextAlign align, int *x1, int *y1,
                       int *width, int *height) {
    int x_offset;
    measure(text, f, width, &x_offset);
    *height = f->height();
    const int xa = (int) align & 0x18, ya = (int) align & 0x07;
    if (xa == (int) TextAlign::RIGHT) *x1 = x - *width - x_offset;
    else if (xa == (int) TextAlign::CENTER_HORIZONTAL) *x1 = x - (*width + x_offset) / 2;
    else *x1 = x;
    if (ya == (int) TextAlign::BOTTOM) *y1 = y - *height;
    else if (ya == (int) TextAlign::BASELINE) *y1 = y - f->ascent;
    else if (ya == (int) TextAlign::CENTER_VERTICAL) *y1 = y - *height / 2;
    else *y1 = y;
  }

  void print(int x, int y, BaseFont *f, Color c, TextAlign align, const char *text) {
    int x1, y1, w, h;
    get_text_bounds(x, y, text, f, align, &x1, &y1, &w, &h);
    int pen = x1;
    for (const char *s = text; *s;) {
      const Glyph *g = f->find(next_cp(s));
      if (g == nullptr) {  // ESPHome outlines the missing character
        rectangle(pen, y1, f->fallback_advance(), f->height(), c);
        pen += f->fallback_advance();
        continue;
      }
      for (int j = 0; j < g->h; j++)
        for (int i = 0; i < g->w; i++)
          if (g->bits[j * g->w + i]) draw_pixel_at(pen + g->ox + i, y1 + g->oy + j, c);
      pen += g->advance;
    }
  }
};

}  // namespace display
}  // namespace esphome
