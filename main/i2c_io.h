#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t i2c_io_init(void);
bool pca9535_present(void);
esp_err_t pca9535_read_ports(uint8_t *port0, uint8_t *port1);

#ifdef __cplusplus
}
#endif
