#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Logical (landscape) display size: 320 wide x 172 tall. The driver keeps an
 * offscreen RGB565 canvas of this size in PSRAM and blits it to the panel in
 * portrait scan order with a transpose, so text is laid out in natural
 * landscape coordinates regardless of how the glass is physically driven. */
#define ST7789_WIDTH  320
#define ST7789_HEIGHT 172

/* Font metrics (glyphs are column-major, bit n = row n, bit 0 = top). */
#define ST7789_FONT_W    5
#define ST7789_FONT_H    8   /* rows incl. descenders */
#define ST7789_CHAR_W(s) ((ST7789_FONT_W + 1) * (s))   /* advance per char */

esp_err_t st7789_init(void);
void st7789_set_backlight(bool on);

/* Canvas API: the following write into the offscreen frame buffer. Call
 * st7789_commit() (from the same task) to push the whole canvas to the panel. */
void st7789_fill_screen(uint16_t color);
void st7789_draw_color_checkerboard(void);
void st7789_fill_rect(int x, int y, int w, int h, uint16_t color);
void st7789_draw_text(const char *text, int x, int y, uint16_t fg, uint16_t bg, int scale);
void st7789_draw_number(int value, int x, int y, uint16_t fg, uint16_t bg, int scale, int max_chars);
void st7789_draw_label_number(const char *label, int value, int x, int y,
                              uint16_t fg, uint16_t bg, int scale, int max_chars);

/* Push the offscreen canvas to the panel (full-frame transpose blit). */
void st7789_commit(void);

#ifdef __cplusplus
}
#endif
