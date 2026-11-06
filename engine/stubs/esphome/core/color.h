#pragma once
// Simulator stand-in for esphome/core/color.h (only what remote_ui.h uses).
#include <cstdint>
namespace esphome {
struct Color {
  uint8_t r = 0, g = 0, b = 0, w = 0;
  constexpr Color() = default;
  constexpr Color(uint8_t r, uint8_t g, uint8_t b, uint8_t w = 0) : r(r), g(g), b(b), w(w) {}
  constexpr bool is_on() const { return r || g || b || w; }
};
}  // namespace esphome
