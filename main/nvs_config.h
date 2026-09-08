#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NVS-backed configuration (namespace "vkey"). Loaded once at boot with
 * defaults; wifi_mqtt reads creds/broker here instead of compile-time macros.
 * The adopted vibetty target prefix is persisted so discovery no longer lets a
 * later random presence hijack the instance (fixes last-wins overwrite). */

const char *nvs_config_wifi_ssid(void);
const char *nvs_config_wifi_pass(void);
const char *nvs_config_broker_uri(void);
const char *nvs_config_target_prefix(void);   /* "" when unset */
bool nvs_config_broker_stored(void);          /* explicit 'broker' saved? */

esp_err_t nvs_config_init(void);
esp_err_t nvs_config_save_broker(const char *uri);
esp_err_t nvs_config_save_target_prefix(const char *prefix);

#ifdef __cplusplus
}
#endif
