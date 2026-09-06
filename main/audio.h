#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_init(void);
esp_err_t audio_capture_read(int16_t *buf, size_t samples, size_t *samples_read);

#ifdef __cplusplus
}
#endif
