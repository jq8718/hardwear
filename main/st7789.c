#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "st7789.h"

#define PIN_MOSI 7
#define PIN_SCLK 6
#define PIN_CS   10
#define PIN_DC   9
#define PIN_RST  8
#define PIN_BLK  26

#define LCD_HOST     SPI2_HOST
#define LCD_SPI_FREQ 40000000

#define COLUMN_OFFSET 34
#define CELL_SIZE     20

#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

static const char *TAG = "st7789";

static spi_device_handle_t s_spi = NULL;

static uint16_t s_row[ST7789_WIDTH] __attribute__((aligned(4)));

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

static void st7789_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    st7789_send_cmd(0x2A);
    st7789_send_data16(x0 + COLUMN_OFFSET);
    st7789_send_data16(x1 + COLUMN_OFFSET);

    st7789_send_cmd(0x2B);
    st7789_send_data16(y0);
    st7789_send_data16(y1);

    st7789_send_cmd(0x2C);
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
        .max_transfer_sz = ST7789_WIDTH * 2,
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

    ESP_LOGI(TAG, "ST7789 initialized, backlight on");
    return ESP_OK;
}

void st7789_set_backlight(bool on)
{
    gpio_set_level(PIN_BLK, on ? 1 : 0);
}

static void st7789_flush(const uint8_t *data, size_t len)
{
    spi_transaction_t t = {0};
    t.length = len * 8;
    t.tx_buffer = data;
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
}

void st7789_fill_screen(uint16_t color)
{
    st7789_set_window(0, 0, ST7789_WIDTH - 1, ST7789_HEIGHT - 1);

    for (int x = 0; x < ST7789_WIDTH; x++) {
        s_row[x] = color;
    }

    gpio_set_level(PIN_DC, 1);
    gpio_set_level(PIN_CS, 0);
    for (int y = 0; y < ST7789_HEIGHT; y++) {
        st7789_flush((const uint8_t *)s_row, sizeof(s_row));
    }
    gpio_set_level(PIN_CS, 1);
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

    st7789_set_window(0, 0, ST7789_WIDTH - 1, ST7789_HEIGHT - 1);

    gpio_set_level(PIN_DC, 1);
    gpio_set_level(PIN_CS, 0);

    for (int y = 0; y < ST7789_HEIGHT; y++) {
        int cy = y / CELL_SIZE;
        for (int x = 0; x < ST7789_WIDTH; x++) {
            int cx = x / CELL_SIZE;
            s_row[x] = palette[(cx + cy) % ncolors];
        }
        st7789_flush((const uint8_t *)s_row, sizeof(s_row));
    }

    gpio_set_level(PIN_CS, 1);
}

#define FONT_WIDTH  5
#define FONT_HEIGHT 7

static const uint8_t font_space[FONT_WIDTH] = {0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t font_minus[FONT_WIDTH] = {0x08, 0x08, 0x08, 0x08, 0x08};
static const uint8_t font_colon[FONT_WIDTH] = {0x00, 0x00, 0x22, 0x00, 0x00};
static const uint8_t font_0[FONT_WIDTH] = {0x3E, 0x41, 0x41, 0x41, 0x3E};
static const uint8_t font_1[FONT_WIDTH] = {0x00, 0x42, 0x7F, 0x40, 0x00};
static const uint8_t font_2[FONT_WIDTH] = {0x42, 0x61, 0x51, 0x49, 0x46};
static const uint8_t font_3[FONT_WIDTH] = {0x22, 0x41, 0x49, 0x49, 0x36};
static const uint8_t font_4[FONT_WIDTH] = {0x18, 0x14, 0x12, 0x7F, 0x10};
static const uint8_t font_5[FONT_WIDTH] = {0x27, 0x45, 0x45, 0x45, 0x39};
static const uint8_t font_6[FONT_WIDTH] = {0x3E, 0x49, 0x49, 0x49, 0x32};
static const uint8_t font_7[FONT_WIDTH] = {0x01, 0x71, 0x09, 0x05, 0x03};
static const uint8_t font_8[FONT_WIDTH] = {0x36, 0x49, 0x49, 0x49, 0x36};
static const uint8_t font_9[FONT_WIDTH] = {0x26, 0x49, 0x49, 0x49, 0x3E};
static const uint8_t font_E[FONT_WIDTH] = {0x7F, 0x49, 0x49, 0x49, 0x41};
static const uint8_t font_N[FONT_WIDTH] = {0x7F, 0x02, 0x04, 0x08, 0x7F};
static const uint8_t font_C[FONT_WIDTH] = {0x3E, 0x41, 0x41, 0x41, 0x22};
static const uint8_t font_A[FONT_WIDTH] = {0x7E, 0x09, 0x09, 0x09, 0x7E};
static const uint8_t font_R[FONT_WIDTH] = {0x7F, 0x09, 0x19, 0x29, 0x46};
static const uint8_t font_W[FONT_WIDTH] = {0x3F, 0x40, 0x38, 0x40, 0x3F};

static const uint8_t *font_glyph(char c)
{
    switch (c) {
    case ' ': return font_space;
    case '-': return font_minus;
    case ':': return font_colon;
    case '0': return font_0;
    case '1': return font_1;
    case '2': return font_2;
    case '3': return font_3;
    case '4': return font_4;
    case '5': return font_5;
    case '6': return font_6;
    case '7': return font_7;
    case '8': return font_8;
    case '9': return font_9;
    case 'E': return font_E;
    case 'N': return font_N;
    case 'C': return font_C;
    case 'A': return font_A;
    case 'R': return font_R;
    case 'W': return font_W;
    default: return font_space;
    }
}

void st7789_fill_rect(int x, int y, int w, int h, uint16_t color)
{
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

    st7789_set_window(x, y, x + w - 1, y + h - 1);

    for (int i = 0; i < w; i++) {
        s_row[i] = color;
    }

    gpio_set_level(PIN_DC, 1);
    gpio_set_level(PIN_CS, 0);
    for (int r = 0; r < h; r++) {
        st7789_flush((const uint8_t *)s_row, (size_t)w * 2);
    }
    gpio_set_level(PIN_CS, 1);
}

void st7789_draw_text(const char *text, int x, int y, uint16_t fg, uint16_t bg, int scale)
{
    int chars = (int)strlen(text);
    if (chars == 0) {
        return;
    }

    int char_step = (FONT_WIDTH + 1) * scale;
    int str_w = chars * char_step;
    int str_h = FONT_HEIGHT * scale;

    st7789_set_window(x, y, x + str_w - 1, y + str_h - 1);

    gpio_set_level(PIN_DC, 1);
    gpio_set_level(PIN_CS, 0);

    // Stream the whole string in one continuous write: one row at a time,
    // each glyph row repeated `scale` times vertically.
    for (int r = 0; r < FONT_HEIGHT; r++) {
        for (int sy = 0; sy < scale; sy++) {
            int n = 0;
            for (int i = 0; i < chars; i++) {
                const uint8_t *glyph = font_glyph(text[i]);
                for (int c = 0; c < FONT_WIDTH; c++) {
                    uint16_t color = (glyph[c] & (1 << r)) ? fg : bg;
                    for (int sx = 0; sx < scale; sx++) {
                        s_row[n++] = color;
                    }
                }
                for (int sp = 0; sp < scale; sp++) {
                    s_row[n++] = bg;
                }
            }
            st7789_flush((const uint8_t *)s_row, (size_t)n * 2);
        }
    }

    gpio_set_level(PIN_CS, 1);
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
