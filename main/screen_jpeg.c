/* screen_jpeg.c -- decode vibetty /screen JPEG frames onto the LCD body.
 *
 * Threading: wifi_mqtt feeds fragments (MQTT task) into s_acc. When a whole
 * frame has arrived it is copied to s_pend under a mutex. The app task's
 * screen_jpeg_poll() takes that mutex, decodes s_pend with TJpgDec (blitting
 * each MCU rectangle straight into the offscreen canvas via
 * st7789_blit_rgb565), commits and frees the slot. Holding the mutex across
 * the decode makes the two buffers race-free: if a newer frame completes while
 * a decode is running the MQTT task simply overwrites s_pend afterwards
 * (latest-wins). */

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "st7789.h"
#include "lcd_vt.h"
#include "tjpgd.h"
#include "screen_jpeg.h"

#define TAG "screen_jpeg"

/* One 320x(H-LCD_HEADER_H) screen JPEG never approaches this; the cap only
 * keeps the two PSRAM buffers at a fixed size. A frame bigger than this is
 * dropped (a single MQTT publish, ~unlikely at this resolution). */
#define MAX_FRAME (256 * 1024)

/* Minimum wall time between decodes: keeps a chatty mirror from pegging the
 * single core at decode throughput (~few fps is plenty for a terminal). */
#define DECODE_MIN_MS 100

/* Decode pool for TJpgDec (huffman/quant tables + MCU work buffers). */
#define POOL_SIZE (32 * 1024)

/* Mirror maps into the canvas body below the status header. */
#define IMG_Y LCD_HEADER_H

static uint8_t *s_acc;              /* MQTT-task accumulation buffer */
static uint8_t *s_pend;             /* completed frame awaiting decode */
static uint8_t *s_pool;             /* TJpgDec working pool */
static SemaphoreHandle_t s_lock;

static size_t s_acc_pos;
static size_t s_acc_total;
static bool   s_acc_overflow;
static size_t s_pend_len;
static uint32_t s_last_decode_ms;

typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t pos;
} jsrc_t;

static size_t jpeg_infunc(JDEC *jd, uint8_t *dst, size_t ndis)
{
    jsrc_t *s = (jsrc_t *) jd->device;
    size_t n = 0;
    if (s->pos < s->len) {
        n = s->len - s->pos;
        if (n > ndis) {
            n = ndis;
        }
        /* dst==NULL is TJpgDec's "skip ndis bytes" call (unknown marker
         * segments): advance the stream without writing anywhere. */
        if (dst) {
            memcpy(dst, s->buf + s->pos, n);
        }
        s->pos += n;
    }
    return n;
}

static int jpeg_outfunc(JDEC *jd, void *bitmap, JRECT *rect)
{
    (void) jd;
    st7789_blit_rgb565(rect->left, rect->top + IMG_Y,
                       (const uint16_t *) bitmap,
                       rect->right - rect->left + 1,
                       rect->bottom - rect->top + 1);
    return 1;   /* continue decoding */
}

void screen_jpeg_init(void)
{
    s_acc  = heap_caps_malloc(MAX_FRAME, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pend = heap_caps_malloc(MAX_FRAME, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pool = heap_caps_malloc(POOL_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_acc || !s_pend || !s_pool) {
        ESP_LOGE(TAG, "PSRAM alloc failed (acc=%p pend=%p pool=%p)",
                 s_acc, s_pend, s_pool);
        s_acc = s_pend = s_pool = NULL;
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    s_pend_len = 0;
    s_last_decode_ms = 0;
    ESP_LOGI(TAG, "screen_jpeg ready (%u KB acc + %u KB pend + %u KB pool, PSRAM)",
             MAX_FRAME / 1024, MAX_FRAME / 1024, POOL_SIZE / 1024);
}

void screen_jpeg_feed(const uint8_t *data, size_t len, size_t offset, size_t total)
{
    if (!s_acc) {
        return;
    }
    if (offset == 0) {          /* first fragment of a new frame */
        s_acc_pos = 0;
        s_acc_total = total;
        s_acc_overflow = total > MAX_FRAME;
        if (s_acc_overflow) {
            ESP_LOGW(TAG, "screen JPEG %d bytes over %d cap, dropping",
                     (int) total, MAX_FRAME);
        }
    }
    if (s_acc_overflow || offset + len > MAX_FRAME) {
        return;
    }
    memcpy(s_acc + offset, data, len);
    s_acc_pos = offset + len;

    if (s_acc_pos >= s_acc_total) {     /* whole frame received */
        /* Non-blocking: if a decode is running (mutex held by the app task),
         * drop this frame rather than stall the MQTT event task. */
        if (xSemaphoreTake(s_lock, 0) == pdTRUE) {
            memcpy(s_pend, s_acc, s_acc_pos);
            s_pend_len = s_acc_pos;
            xSemaphoreGive(s_lock);
        }
    }
}

void screen_jpeg_reset(void)
{
    if (!s_lock) {
        return;
    }
    if (xSemaphoreTake(s_lock, 0) == pdTRUE) {
        s_pend_len = 0;
        s_acc_pos = 0;
        s_acc_total = 0;
        s_acc_overflow = false;
        xSemaphoreGive(s_lock);
    }
}

static uint32_t now_ms(void)
{
    return (uint32_t) (xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void screen_jpeg_poll(void)
{
    if (!s_lock || s_pend_len == 0) {
        return;
    }
    uint32_t now = now_ms();
    if (now - s_last_decode_ms < DECODE_MIN_MS) {
        return;
    }
    /* Hold the mutex across the decode so the MQTT task cannot overwrite
     * s_pend mid-decode; a frame that completes meanwhile waits and then
     * replaces the slot (latest wins). */
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    size_t len = s_pend_len;
    bool ok = false;
    if (len >= 2) {
        jsrc_t src = { s_pend, len, 0 };
        JDEC jd;
        JRESULT rc = jpeg_prepare(&jd, jpeg_infunc, s_pool, POOL_SIZE, &src);
        if (rc == JDR_OK) {
            rc = jpeg_decomp(&jd, jpeg_outfunc, 0);
            if (rc == JDR_OK) {
                ESP_LOGI(TAG, "frame %ux%u %u bytes",
                         jd.width, jd.height, (unsigned) len);
                ok = true;
            }
        }
        if (!ok) {
            ESP_LOGW(TAG, "jpeg decode failed rc=%d (%u bytes)", rc,
                     (unsigned) len);
        }
    }
    s_pend_len = 0;
    s_last_decode_ms = now;
    xSemaphoreGive(s_lock);

    if (ok) {
        st7789_commit();
    }
}
