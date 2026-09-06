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
#define COLOR_ENC3     0xF81F
#define COLOR_RAW      0xFFE0
#define COLOR_ACC      0xFFFF
#define COLOR_DIVIDER  0x39E7

#define TEXT_SCALE     2
#define RAW_MAX_CHARS  6
#define ACC_MAX_CHARS  9

#define TEXT_X 10

#define ENC1_LABEL_Y 6
#define ENC1_RAW_Y   38
#define ENC1_ACC_Y   70
#define DIVIDER1_Y   102
#define ENC2_LABEL_Y 108
#define ENC2_RAW_Y   140
#define ENC2_ACC_Y   172
#define DIVIDER2_Y   204
#define ENC3_LABEL_Y 210
#define ENC3_RAW_Y   242
#define ENC3_ACC_Y   274

#define ENC_COUNT 3

static const char *TAG = "ws2812";

typedef struct {
    const char *label;
    int y_label;
    int y_raw;
    int y_acc;
    uint16_t color;
} enc_view_t;

static const enc_view_t s_enc_views[ENC_COUNT] = {
    {"ENC1", ENC1_LABEL_Y, ENC1_RAW_Y, ENC1_ACC_Y, COLOR_ENC1},
    {"ENC2", ENC2_LABEL_Y, ENC2_RAW_Y, ENC2_ACC_Y, COLOR_ENC2},
    {"ENC3", ENC3_LABEL_Y, ENC3_RAW_Y, ENC3_ACC_Y, COLOR_ENC3},
};

static void enc_get(int idx, int *raw, int *acc)
{
    switch (idx) {
    case 0:
        *raw = encoder1_get_raw();
        *acc = encoder1_get_count();
        break;
    case 1:
        *raw = encoder2_get_raw();
        *acc = encoder2_get_count();
        break;
    default:
        *raw = encoder3_get_raw();
        *acc = encoder3_get_count();
        break;
    }
}

static void draw_encoder_layout(void)
{
    st7789_fill_screen(COLOR_BG);

    for (int i = 0; i < ENC_COUNT; i++) {
        st7789_draw_text(s_enc_views[i].label, TEXT_X, s_enc_views[i].y_label,
                         s_enc_views[i].color, COLOR_BG, TEXT_SCALE);
        st7789_draw_label_number("RAW ", 0, TEXT_X, s_enc_views[i].y_raw,
                                 COLOR_RAW, COLOR_BG, TEXT_SCALE, RAW_MAX_CHARS);
        st7789_draw_label_number("ACC ", 0, TEXT_X, s_enc_views[i].y_acc,
                                 COLOR_ACC, COLOR_BG, TEXT_SCALE, ACC_MAX_CHARS);
    }

    st7789_fill_rect(0, DIVIDER1_Y, ST7789_WIDTH, 2, COLOR_DIVIDER);
    st7789_fill_rect(0, DIVIDER2_Y, ST7789_WIDTH, 2, COLOR_DIVIDER);
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

    int last_raw[ENC_COUNT] = {0};
    int last_acc[ENC_COUNT] = {0};

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

        for (int i = 0; i < ENC_COUNT; i++) {
            int raw;
            int acc;
            enc_get(i, &raw, &acc);

            if (raw != last_raw[i]) {
                st7789_draw_label_number("RAW ", raw, TEXT_X, s_enc_views[i].y_raw,
                                         COLOR_RAW, COLOR_BG, TEXT_SCALE, RAW_MAX_CHARS);
                last_raw[i] = raw;
            }
            if (acc != last_acc[i]) {
                st7789_draw_label_number("ACC ", acc, TEXT_X, s_enc_views[i].y_acc,
                                         COLOR_ACC, COLOR_BG, TEXT_SCALE, ACC_MAX_CHARS);
                last_acc[i] = acc;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(WS2812_FRAME_DELAY_MS));
    }
}
