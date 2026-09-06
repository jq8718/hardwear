#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode vibetty /screen JPEG frames onto the LCD canvas.
 *
 * wifi_mqtt feeds raw fragments from the MQTT event (the JPEG may span several
 * MQTT_EVENT_DATA events when it is larger than the 8 KB client buffer) and
 * screen_jpeg reassembles, decodes with TJpgDec and blits into the body of the
 * canvas below the status header. Decode runs on the app task via
 * screen_jpeg_poll() so all canvas/SPI access stays single-threaded. */
void screen_jpeg_init(void);

/* Feed one fragment of a screen JPEG. A new frame starts when offset==0;
 * total is the frame's expected payload length (event->total_data_len). */
void screen_jpeg_feed(const uint8_t *data, size_t len, size_t offset, size_t total);

/* Drop any reassembled/undecoded frame (called when the link goes away so a
 * stale frame cannot flash on a later reconnect). */
void screen_jpeg_reset(void);

/* App task: if a complete frame is waiting, decode it, blit it and commit.
 * Call only while the lcd_vt is in pixels (mirror) mode. */
void screen_jpeg_poll(void);

#ifdef __cplusplus
}
#endif
