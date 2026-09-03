#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST7789_WIDTH  172
#define ST7789_HEIGHT 320

esp_err_t st7789_init(void);
void st7789_set_backlight(bool on);
void st7789_fill_screen(uint16_t color);
void st7789_draw_color_checkerboard(void);

#ifdef __cplusplus
}
#endif
