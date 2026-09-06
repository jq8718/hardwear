#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t encoder_init(void);
int encoder1_get_count(void);
int encoder2_get_count(void);
int encoder3_get_count(void);
int encoder1_get_raw(void);
int encoder2_get_raw(void);
int encoder3_get_raw(void);

#ifdef __cplusplus
}
#endif
