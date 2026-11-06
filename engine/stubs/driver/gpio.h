#pragma once
// Simulator stand-ins: the power-latch helpers compile but do nothing.
enum gpio_num_t { GPIO_NUM_4 = 4, GPIO_NUM_5 = 5, GPIO_NUM_6 = 6, GPIO_NUM_38 = 38, GPIO_NUM_39 = 39,
                  GPIO_NUM_45 = 45, GPIO_NUM_46 = 46 };
inline int gpio_hold_en(gpio_num_t) { return 0; }
inline int gpio_hold_dis(gpio_num_t) { return 0; }
inline void gpio_deep_sleep_hold_en() {}
inline void gpio_deep_sleep_hold_dis() {}
