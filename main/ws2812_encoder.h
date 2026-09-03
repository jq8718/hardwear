#pragma once

#include <stdint.h>
#include "driver/rmt_encoder.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t resolution;
} ws2812_encoder_config_t;

esp_err_t ws2812_encoder_new(const ws2812_encoder_config_t *config,
                             rmt_encoder_handle_t *ret_encoder);

#ifdef __cplusplus
}
#endif
