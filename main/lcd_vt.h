#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LCDVT_COLS/ROWS are also the PTY size vibetty is told to run the session at
 * (via the sync control), so the mirrored grid matches the panel exactly.
 * Landscape 320x172 at 2x text: 26 cols of 12 px + margin, 8 rows in the
 * body below the ~20 px status header. */
#define LCDVT_COLS 26
#define LCDVT_ROWS 8

/* Status-header height in px at the top of the canvas. When a JPEG-capable
 * vibetty instance is mirrored the decoded image fills the body below this. */
#define LCD_HEADER_H 20

typedef enum {
    LCD_ST_BOOT = 0,
    LCD_ST_CONNECTING,
    LCD_ST_WAITING,   /* agent idle, waiting for input */
    LCD_ST_WORKING,   /* agent working on a task */
    LCD_ST_OFFLINE,
} lcd_vt_state_t;

void lcd_vt_init(void);
/* Update the colored status header. */
void lcd_vt_set_status(lcd_vt_state_t st, const char *title);
/* Switch to/from bitmap mirror mode. In pixels mode the text grid is blanked
 * and poll() only maintains the status header; screen_jpeg draws the body. */
void lcd_vt_set_pixels(bool on);
bool lcd_vt_pixels_mode(void);
/* terminal API, ANSI-fed: tag 0x00 baseline resets the grid then replays;
 * tag 0x01 incremental appends raw ANSI bytes. */
void lcd_vt_feed_baseline(const uint8_t *data, size_t len);
void lcd_vt_feed(const uint8_t *data, size_t len);
/* Draw any dirty rows; call periodically from the app task (not MQTT task). */
void lcd_vt_poll(void);

#ifdef __cplusplus
}
#endif
