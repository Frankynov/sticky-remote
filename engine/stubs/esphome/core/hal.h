#pragma once
// Simulator stand-in: millis() from the host's monotonic clock.
#include <chrono>
#include <cstdint>
namespace esphome {
inline uint32_t millis() {
  using namespace std::chrono;
  static const auto t0 = steady_clock::now();
  return (uint32_t) duration_cast<milliseconds>(steady_clock::now() - t0).count();
}
}  // namespace esphome
