#pragma once
enum esp_sleep_pd_domain_t { ESP_PD_DOMAIN_RTC_PERIPH };
enum esp_sleep_pd_option_t { ESP_PD_OPTION_ON };
inline int esp_sleep_pd_config(esp_sleep_pd_domain_t, esp_sleep_pd_option_t) { return 0; }
