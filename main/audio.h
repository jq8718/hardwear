#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_init(void);
esp_err_t audio_capture_read(int16_t *buf, size_t samples, size_t *samples_read);

/* Software PDM -> 16 kHz PCM decimation (stage-7 voice bypass).
 * audio_pdm_decoder_size() gives the state size to pass as *st. */
size_t audio_pdm_decoder_size(void);
esp_err_t audio_pdm_decimate(const int16_t *raw, size_t slots, int16_t *out,
                             size_t out_cap, size_t *out_n, void *st);

#ifdef __cplusplus
}
#endif
