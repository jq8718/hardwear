#include <stdint.h>
#include <stdbool.h>

#include "driver/rmt_tx.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "encoder.h"
#include "st7789.h"
#include "ws2812_encoder.h"

#define WS2812_GPIO             27
#define WS2812_RMT_RESOLUTION_HZ 10000000
#define WS2812_RAINBOW_STEP     3
#define WS2812_FRAME_DELAY_MS   20
#define WS2812_BRIGHTNESS       32

#define COLOR_BG       0x0000
#define COLOR_ENC1     0x07E0
#define COLOR_ENC2     0x07FF
#define COLOR_RAW      0xFFE0
#define COLOR_ACC      0xFFFF
#define COLOR_DIVIDER  0x39E7

#define TEXT_SCALE     2
#define RAW_MAX_CHARS  6
#define ACC_MAX_CHARS  9

#define TEXT_X 10

#define ENC1_LABEL_Y 10
#define ENC1_RAW_Y   46
#define ENC1_ACC_Y   82
#define DIVIDER_Y    150

#define ENC2_LABEL_Y 166
#define ENC2_RAW_Y   202
#define ENC2_ACC_Y   238

static const char *TAG = "ws2812";

static void draw_encoder_layout(void)
{
    st7789_fill_screen(COLOR_BG);

    st7789_draw_text("ENC1", TEXT_X, ENC1_LABEL_Y, COLOR_ENC1, COLOR_BG, TEXT_SCALE);
    st7789_draw_label_number("RAW ", 0, TEXT_X, ENC1_RAW_Y, COLOR_RAW, COLOR_BG, TEXT_SCALE, RAW_MAX_CHARS);
    st7789_draw_label_number("ACC ", 0, TEXT_X, ENC1_ACC_Y, COLOR_ACC, COLOR_BG, TEXT_SCALE, ACC_MAX_CHARS);

    st7789_fill_rect(0, DIVIDER_Y, ST7789_WIDTH, 2, COLOR_DIVIDER);

    st7789_draw_text("ENC2", TEXT_X, ENC2_LABEL_Y, COLOR_ENC2, COLOR_BG, TEXT_SCALE);
    st7789_draw_label_number("RAW ", 0, TEXT_X, ENC2_RAW_Y, COLOR_RAW, COLOR_BG, TEXT_SCALE, RAW_MAX_CHARS);
    st7789_draw_label_number("ACC ", 0, TEXT_X, ENC2_ACC_Y, COLOR_ACC, COLOR_BG, TEXT_SCALE, ACC_MAX_CHARS);
}

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
    ESP_ERROR_CHECK(encoder_init());
    draw_encoder_layout();
    ESP_LOGI(TAG, "LCD encoder display ready");

    int last_raw1 = 0;
    int last_acc1 = 0;
    int last_raw2 = 0;
    int last_acc2 = 0;

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

        int raw1 = encoder1_get_raw();
        int acc1 = encoder1_get_count();
        int raw2 = encoder2_get_raw();
        int acc2 = encoder2_get_count();

        if (raw1 != last_raw1) {
            st7789_draw_label_number("RAW ", raw1, TEXT_X, ENC1_RAW_Y,
                                     COLOR_RAW, COLOR_BG, TEXT_SCALE, RAW_MAX_CHARS);
            last_raw1 = raw1;
        }
        if (acc1 != last_acc1) {
            st7789_draw_label_number("ACC ", acc1, TEXT_X, ENC1_ACC_Y,
                                     COLOR_ACC, COLOR_BG, TEXT_SCALE, ACC_MAX_CHARS);
            last_acc1 = acc1;
        }
        if (raw2 != last_raw2) {
            st7789_draw_label_number("RAW ", raw2, TEXT_X, ENC2_RAW_Y,
                                     COLOR_RAW, COLOR_BG, TEXT_SCALE, RAW_MAX_CHARS);
            last_raw2 = raw2;
        }
        if (acc2 != last_acc2) {
            st7789_draw_label_number("ACC ", acc2, TEXT_X, ENC2_ACC_Y,
                                     COLOR_ACC, COLOR_BG, TEXT_SCALE, ACC_MAX_CHARS);
            last_acc2 = acc2;
        }

        vTaskDelay(pdMS_TO_TICKS(WS2812_FRAME_DELAY_MS));
    }
}
