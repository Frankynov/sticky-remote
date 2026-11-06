#pragma once
// Simulator stand-in for esphome::qr_code::QrCode, on the same qrcodegen
// library (engine/core/qrcodegen) and with the same drawing.
#include <cstdint>
#include <string>
#include "esphome/components/display/display.h"
#include "qrcodegen.h"

namespace esphome {
namespace qr_code {
class QrCode {
 public:
  void set_value(const std::string &value) {
    value_ = value;
    needs_update_ = true;
  }
  void set_ecc(qrcodegen_Ecc ecc) {
    ecc_ = ecc;
    needs_update_ = true;
  }
  void generate_qr_code() {
    uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
    if (!qrcodegen_encodeText(value_.c_str(), temp, qr_, ecc_, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                              qrcodegen_Mask_AUTO, true))
      qr_[0] = 0;
    needs_update_ = false;
  }
  uint8_t get_size() {
    if (needs_update_) generate_qr_code();
    return qr_[0] ? (uint8_t) qrcodegen_getSize(qr_) : 0;
  }
  void draw(display::Display *d, uint16_t x0, uint16_t y0, Color color, int scale) {
    const int n = get_size();
    for (int y = 0; y < n * scale; y++)
      for (int x = 0; x < n * scale; x++)
        if (qrcodegen_getModule(qr_, x / scale, y / scale)) d->draw_pixel_at(x0 + x, y0 + y, color);
  }

 private:
  std::string value_;
  qrcodegen_Ecc ecc_ = qrcodegen_Ecc_LOW;   // ESPHome's default
  bool needs_update_ = true;
  uint8_t qr_[qrcodegen_BUFFER_LEN_MAX] = {0};
};
}  // namespace qr_code

inline void display::Display::qr_code(int x, int y, qr_code::QrCode *qr, Color color, int scale) {
  qr->draw(this, x, y, color, scale);
}
}  // namespace esphome
