#pragma once
// Simulator stand-in for RuntimeImage: an 8-bit grayscale buffer, as the
// firmware's online_image (type GRAYSCALE) decodes into.
#include <cstdint>
#include <vector>
namespace esphome {
namespace runtime_image {
class RuntimeImage {
 public:
  virtual ~RuntimeImage() = default;
  int get_buffer_width() const { return buffer_width_; }
  int get_buffer_height() const { return buffer_height_; }
  bool is_loaded() const { return buffer_ != nullptr; }

 protected:
  uint8_t *buffer_{nullptr};
  int buffer_width_{0};
  int buffer_height_{0};
};
}  // namespace runtime_image
}  // namespace esphome
