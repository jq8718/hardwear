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
void st7789_fill_rect(int x, int y, int w, int h, uint16_t color);
void st7789_draw_text(const char *text, int x, int y, uint16_t fg, uint16_t bg, int scale);
void st7789_draw_number(int value, int x, int y, uint16_t fg, uint16_t bg, int scale, int max_chars);
void st7789_draw_label_number(const char *label, int value, int x, int y, uint16_t fg, uint16_t bg, int scale, int max_chars);

#ifdef __cplusplus
}
#endif
