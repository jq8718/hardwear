#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_mqtt_init(void);
bool wifi_mqtt_connected(void);
esp_err_t wifi_mqtt_send_key(const uint8_t *bytes, size_t len);
esp_err_t wifi_mqtt_send_text(const char *text);

#ifdef __cplusplus
}
#endif
