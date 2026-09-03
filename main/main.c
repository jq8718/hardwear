#include <stdint.h>
#include <stdbool.h>

#include "driver/rmt_tx.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "st7789.h"
#include "ws2812_encoder.h"

#define WS2812_GPIO             27
#define WS2812_RMT_RESOLUTION_HZ 10000000
#define WS2812_RAINBOW_STEP     3
#define WS2812_FRAME_DELAY_MS   20
#define WS2812_BRIGHTNESS       32

static const char *TAG = "ws2812";

static void hsv_to_rgb(uint16_t hue, uint8_t *red, uint8_t *green, uint8_t *blue)
{
    hue %= 360;
    uint16_t offset = (hue % 60) * 255 / 60;
    uint8_t rising = (uint16_t) WS2812_BRIGHTNESS * offset / 255;
    uint8_t falling = (uint16_t) WS2812_BRIGHTNESS * (255 - offset) / 255;

    switch (hue / 60) {
    case 0:
        *red = WS2812_BRIGHTNESS;
        *green = rising;
        *blue = 0;
        break;
    case 1:
        *red = falling;
        *green = WS2812_BRIGHTNESS;
        *blue = 0;
        break;
    case 2:
        *red = 0;
        *green = WS2812_BRIGHTNESS;
        *blue = rising;
        break;
    case 3:
        *red = 0;
        *green = falling;
        *blue = WS2812_BRIGHTNESS;
        break;
    case 4:
        *red = rising;
        *green = 0;
        *blue = WS2812_BRIGHTNESS;
        break;
    default:
        *red = WS2812_BRIGHTNESS;
        *green = 0;
        *blue = falling;
        break;
    }
}

void app_main(void)
{
    // WS2812 receives GRB bytes. Keep the peak brightness low for the single LED.
    uint8_t pixel[3] = {0};
    uint16_t hue = 0;

    rmt_channel_handle_t channel = NULL;
    rmt_tx_channel_config_t channel_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = WS2812_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = WS2812_RMT_RESOLUTION_HZ,
        .trans_queue_depth = 1,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&channel_config, &channel));

    rmt_encoder_handle_t encoder = NULL;
    ws2812_encoder_config_t encoder_config = {
        .resolution = WS2812_RMT_RESOLUTION_HZ,
    };
    ESP_ERROR_CHECK(ws2812_encoder_new(&encoder_config, &encoder));
    ESP_ERROR_CHECK(rmt_enable(channel));

    rmt_transmit_config_t transmit_config = {
        .loop_count = 0,
    };
    ESP_LOGI(TAG, "Memory: internal free=%u, PSRAM total=%u, PSRAM free=%u",
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned) heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "WS2812 rainbow started on GPIO%d", WS2812_GPIO);

    ESP_ERROR_CHECK(st7789_init());
    st7789_draw_color_checkerboard();
    ESP_LOGI(TAG, "LCD color checkerboard drawn");

    while (true) {
        uint8_t red;
        uint8_t green;
        uint8_t blue;
        hsv_to_rgb(hue, &red, &green, &blue);
        pixel[0] = green;
        pixel[1] = red;
        pixel[2] = blue;

        ESP_ERROR_CHECK(rmt_transmit(channel, encoder, pixel, sizeof(pixel),
                                     &transmit_config));
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(channel, portMAX_DELAY));
        hue = (hue + WS2812_RAINBOW_STEP) % 360;
        vTaskDelay(pdMS_TO_TICKS(WS2812_FRAME_DELAY_MS));
    }
}
