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
#define LCD_SPI_FREQ 20000000

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
