#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t input_map_init(void);
void input_map_tick(void);

#ifdef __cplusplus
}
#endif
