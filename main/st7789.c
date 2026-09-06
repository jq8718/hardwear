#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "st7789.h"
#include "font5x7.h"

#define PIN_MOSI 7
#define PIN_SCLK 6
#define PIN_CS   10
#define PIN_DC   9
#define PIN_RST  8
#define PIN_BLK  26

#define LCD_HOST     SPI2_HOST
#define LCD_SPI_FREQ 40000000

/* Physical panel geometry (the glass stays portrait 172x320). */
#define NATIVE_W      172
#define NATIVE_H      320
#define NATIVE_OFFSET 34

/* Landscape orientation of the canvas on the panel.
 * Mapping used (ROT_FLIP=0): native row = logical x (so text reads along the
 * long axis), native column = (NATIVE_W-1) - logical y. Set to 1 to rotate
 * 180 degrees if the image comes up upside down on the bench. */
#define ROT_FLIP 0

#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

static const char *TAG = "st7789";

static spi_device_handle_t s_spi = NULL;

static uint16_t *s_fb;             /* canvas[320][172], [x*ST7789_HEIGHT + y] */
static uint16_t s_row[NATIVE_W] __attribute__((aligned(4)));
static uint8_t s_rowb[NATIVE_W * 2] __attribute__((aligned(4)));

static void st7789_send_cmd(uint8_t cmd)
{
    gpio_set_level(PIN_DC, 0);
    gpio_set_level(PIN_CS, 0);
    spi_transaction_t t = {0};
    t.flags = SPI_TRANS_USE_TXDATA;
    t.length = 8;
    t.tx_data[0] = cmd;
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
    gpio_set_level(PIN_CS, 1);
}

static void st7789_send_data8(uint8_t b)
{
    gpio_set_level(PIN_DC, 1);
    gpio_set_level(PIN_CS, 0);
    spi_transaction_t t = {0};
    t.flags = SPI_TRANS_USE_TXDATA;
    t.length = 8;
    t.tx_data[0] = b;
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
    gpio_set_level(PIN_CS, 1);
}

static void st7789_send_data16(uint16_t v)
{
    gpio_set_level(PIN_DC, 1);
    gpio_set_level(PIN_CS, 0);
    spi_transaction_t t = {0};
    t.flags = SPI_TRANS_USE_TXDATA;
    t.length = 16;
    t.tx_data[0] = (uint8_t)(v >> 8);
    t.tx_data[1] = (uint8_t)(v & 0xFF);
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
    gpio_set_level(PIN_CS, 1);
}

static void st7789_send_data(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;
    }
    gpio_set_level(PIN_DC, 1);
    gpio_set_level(PIN_CS, 0);
    spi_transaction_t t = {0};
    t.length = len * 8;
    t.tx_buffer = data;
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
    gpio_set_level(PIN_CS, 1);
}

static void st7789_flush(const uint8_t *data, size_t len)
{
    spi_transaction_t t = {0};
    t.length = len * 8;
    t.tx_buffer = data;
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
}

static void st7789_reset(void)
{
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

esp_err_t st7789_init(void)
{
    s_fb = heap_caps_malloc(ST7789_WIDTH * ST7789_HEIGHT * sizeof(uint16_t),
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fb) {
        s_fb = malloc(ST7789_WIDTH * ST7789_HEIGHT * sizeof(uint16_t));
    }
    if (!s_fb) {
        ESP_LOGE(TAG, "no memory for canvas (%d px)", ST7789_WIDTH * ST7789_HEIGHT);
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_CS) | (1ULL << PIN_DC) |
                        (1ULL << PIN_RST) | (1ULL << PIN_BLK),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    gpio_set_level(PIN_CS, 1);
    gpio_set_level(PIN_DC, 1);
    gpio_set_level(PIN_RST, 1);
    gpio_set_level(PIN_BLK, 0);

    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = NATIVE_W * 2,
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = LCD_SPI_FREQ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &devcfg, &s_spi));

    st7789_reset();
    st7789_set_backlight(true);

    // ST7789V init sequence (mirrors the STM32 reference project).
    st7789_send_cmd(0x11);              // sleep out
    st7789_send_cmd(0x36);
    st7789_send_data8(0xC0);            // MADCTL: MY|MX, portrait
    st7789_send_cmd(0x3A);
    st7789_send_data8(0x05);            // 16-bit RGB565

    static uint8_t porctrl[5] __attribute__((aligned(4))) = {
        0x0C, 0x0C, 0x00, 0x33, 0x33,
    };
    st7789_send_cmd(0xB2);
    st7789_send_data(porctrl, sizeof(porctrl));

    st7789_send_cmd(0xB7);
    st7789_send_data8(0x35);
    st7789_send_cmd(0xBB);
    st7789_send_data8(0x35);
    st7789_send_cmd(0xC0);
    st7789_send_data8(0x2C);
    st7789_send_cmd(0xC2);
    st7789_send_data8(0x01);
    st7789_send_cmd(0xC3);
    st7789_send_data8(0x13);
    st7789_send_cmd(0xC4);
    st7789_send_data8(0x20);
    st7789_send_cmd(0xC6);
    st7789_send_data8(0x0F);

    st7789_send_cmd(0xD0);
    st7789_send_data8(0xA4);
    st7789_send_data8(0xA1);
    st7789_send_cmd(0xD6);
    st7789_send_data8(0xA1);

    static uint8_t pvgam[14] __attribute__((aligned(4))) = {
        0xF0, 0x00, 0x04, 0x04, 0x04, 0x05, 0x29, 0x33,
        0x3E, 0x38, 0x12, 0x12, 0x28, 0x30,
    };
    st7789_send_cmd(0xE0);
    st7789_send_data(pvgam, sizeof(pvgam));

    static uint8_t nvgam[14] __attribute__((aligned(4))) = {
        0xF0, 0x07, 0x0A, 0x0D, 0x0B, 0x07, 0x28, 0x33,
        0x3E, 0x36, 0x14, 0x14, 0x29, 0x32,
    };
    st7789_send_cmd(0xE1);
    st7789_send_data(nvgam, sizeof(nvgam));

    st7789_send_cmd(0x21);              // display inversion ON
    st7789_send_cmd(0x11);              // sleep out (again)
    vTaskDelay(pdMS_TO_TICKS(120));
    st7789_send_cmd(0x29);              // display ON

    ESP_LOGI(TAG, "ST7789 canvas %dx%d initialized (%s), backlight on",
             ST7789_WIDTH, ST7789_HEIGHT,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) ? "psram fb" : "internal fb");
    return ESP_OK;
}

void st7789_set_backlight(bool on)
{
    gpio_set_level(PIN_BLK, on ? 1 : 0);
}

void st7789_blit_rgb565(int x, int y, const uint16_t *pix, int w, int h)
{
    if (!s_fb || !pix || w <= 0 || h <= 0) {
        return;
    }
    int x0 = x, y0 = y;
    int x1 = x + w - 1, y1 = y + h - 1;
    if (x1 < 0 || y1 < 0 || x0 >= ST7789_WIDTH || y0 >= ST7789_HEIGHT) {
        return;
    }
    if (x0 < 0) { pix += -x0; x0 = 0; }      /* skip clipped left pixels */
    if (y0 < 0) { pix += (-y0) * w; y0 = 0; }
    if (x1 >= ST7789_WIDTH) x1 = ST7789_WIDTH - 1;
    if (y1 >= ST7789_HEIGHT) y1 = ST7789_HEIGHT - 1;

    for (int yy = y0; yy <= y1; yy++) {
        const uint16_t *row = pix + (yy - y0) * w;
        for (int xx = x0; xx <= x1; xx++) {
            s_fb[xx * ST7789_HEIGHT + yy] = row[xx - x0];
        }
    }
}

void st7789_commit(void)
{
    if (!s_fb) {
        return;
    }
    /* Paint the full native panel: set the window once (native cols +offset,
     * all rows), then stream one native row at a time. Native row R corresponds
     * to logical x = R; each native column C is logical y = NATIVE_W-1-C (or C
     * when ROT_FLIP), giving the landscape transpose. */
    st7789_send_cmd(0x2A);
    st7789_send_data16(NATIVE_OFFSET);
    st7789_send_data16(NATIVE_OFFSET + NATIVE_W - 1);
    st7789_send_cmd(0x2B);
    st7789_send_data16(0);
    st7789_send_data16(NATIVE_H - 1);
    st7789_send_cmd(0x2C);

    gpio_set_level(PIN_DC, 1);
    gpio_set_level(PIN_CS, 0);
    for (int R = 0; R < NATIVE_H; R++) {
        const uint16_t *fbrow = &s_fb[R * ST7789_HEIGHT];
#if ROT_FLIP
        memcpy(s_row, fbrow, sizeof(s_row));
#else
        for (int C = 0; C < NATIVE_W; C++) {
            s_row[C] = fbrow[NATIVE_W - 1 - C];
        }
#endif
        /* ST7789 samples the high byte of each 16-bit pixel first, so the
         * native little-endian s_row must be byte-swapped on the wire. */
        for (int C = 0; C < NATIVE_W; C++) {
            s_rowb[2 * C]     = (uint8_t)(s_row[C] >> 8);
            s_rowb[2 * C + 1] = (uint8_t)(s_row[C] & 0xFF);
        }
        st7789_flush(s_rowb, sizeof(s_rowb));
    }
    gpio_set_level(PIN_CS, 1);
}

void st7789_fill_screen(uint16_t color)
{
    if (!s_fb) {
        return;
    }
    for (size_t i = 0; i < (size_t)ST7789_WIDTH * ST7789_HEIGHT; i++) {
        s_fb[i] = color;
    }
}

void st7789_draw_color_checkerboard(void)
{
    static const uint16_t palette[] = {
        RGB565(255, 0, 0),
        RGB565(255, 165, 0),
        RGB565(255, 255, 0),
        RGB565(0, 255, 0),
        RGB565(0, 255, 255),
        RGB565(0, 0, 255),
        RGB565(255, 0, 255),
        RGB565(255, 255, 255),
    };
    const int ncolors = sizeof(palette) / sizeof(palette[0]);
    const int cell = 20;
    for (int y = 0; y < ST7789_HEIGHT; y++) {
        int cy = y / cell;
        for (int x = 0; x < ST7789_WIDTH; x++) {
            int cx = x / cell;
            s_fb[x * ST7789_HEIGHT + y] = palette[(cx + cy) % ncolors];
        }
    }
}

void st7789_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_fb) {
        return;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > ST7789_WIDTH) {
        w = ST7789_WIDTH - x;
    }
    if (y + h > ST7789_HEIGHT) {
        h = ST7789_HEIGHT - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int yy = y; yy < y + h; yy++) {
        uint16_t *rowp = &s_fb[x * ST7789_HEIGHT + yy];
        for (int xx = 0; xx < w; xx++) {
            rowp[xx * ST7789_HEIGHT] = color;
        }
    }
}

static const uint8_t *font_glyph(char c)
{
    int idx = (int)((unsigned char)c) - FONT_GLYPH_OFFSET;
    if (idx < 0 || idx >= FONT_NUM_GLYPHS) {
        idx = 0;   /* space */
    }
    return font5x7[idx];
}

void st7789_draw_text(const char *text, int x, int y, uint16_t fg, uint16_t bg, int scale)
{
    if (!s_fb) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }
    int chars = (int)strlen(text);
    if (chars == 0) {
        return;
    }
    int char_step = (ST7789_FONT_W + 1) * scale;

    for (int r = 0; r < ST7789_FONT_H; r++) {
        for (int sy = 0; sy < scale; sy++) {
            int yy = y + r * scale + sy;
            if (yy < 0 || yy >= ST7789_HEIGHT) {
                continue;
            }
            for (int i = 0; i < chars; i++) {
                const uint8_t *glyph = font_glyph(text[i]);
                for (int c = 0; c < ST7789_FONT_W; c++) {
                    uint16_t color = (glyph[c] & (1 << r)) ? fg : bg;
                    for (int sx = 0; sx < scale; sx++) {
                        int xx = x + i * char_step + c * scale + sx;
                        if (xx < 0 || xx >= ST7789_WIDTH) {
                            continue;
                        }
                        s_fb[xx * ST7789_HEIGHT + yy] = color;
                    }
                }
                if ((x + i * char_step + ST7789_FONT_W * scale) < ST7789_WIDTH) {
                    for (int sx = 0; sx < scale; sx++) {
                        int xx = x + i * char_step + ST7789_FONT_W * scale + sx;
                        if (xx >= 0 && xx < ST7789_WIDTH) {
                            s_fb[xx * ST7789_HEIGHT + yy] = bg;
                        }
                    }
                }
            }
        }
    }
}

void st7789_draw_number(int value, int x, int y, uint16_t fg, uint16_t bg, int scale, int max_chars)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%-*d", max_chars, value);
    st7789_draw_text(buf, x, y, fg, bg, scale);
}

void st7789_draw_label_number(const char *label, int value, int x, int y,
                              uint16_t fg, uint16_t bg, int scale, int max_chars)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%s%-*d", label, max_chars, value);
    st7789_draw_text(buf, x, y, fg, bg, scale);
}
