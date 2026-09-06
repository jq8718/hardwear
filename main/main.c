#include <stdint.h>
#include <stdbool.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "encoder.h"
#include "audio.h"
#include "i2c_io.h"
#include "input_map.h"
#include "power_mgmt.h"
#include "state_led.h"
#include "st7789.h"
#include "wifi_mqtt.h"
#include "nvs_config.h"

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

#define LOOP_DELAY_MS 20

static const char *TAG = "main";

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

void app_main(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    ESP_LOGI(TAG, "firmware v%s (ota slot current)", desc ? desc->version : "?");

    uint32_t causes = esp_sleep_get_wakeup_causes();
    if (causes & BIT(ESP_SLEEP_WAKEUP_UNDEFINED)) {
        ESP_LOGI(TAG, "cold boot");
    } else {
        const char *w = "?";
        if (causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) {
            w = "encoder GPIO (EXT1)";
        } else if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
            w = "timer";
        } else if (causes & BIT(ESP_SLEEP_WAKEUP_GPIO)) {
            w = "GPIO";
        }
        ESP_LOGI(TAG, "wake from deep sleep: %s (causes 0x%x)", w,
                 (unsigned) causes);
    }

    ESP_LOGI(TAG, "Memory: internal free=%u, PSRAM total=%u, PSRAM free=%u",
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned) heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    ESP_ERROR_CHECK(nvs_config_init());

    ESP_ERROR_CHECK(state_led_init());
    ESP_ERROR_CHECK(st7789_init());
    ESP_ERROR_CHECK(encoder_init());
    ESP_ERROR_CHECK(i2c_io_init());
    ESP_ERROR_CHECK(audio_init());
    ESP_ERROR_CHECK(input_map_init());
    ESP_ERROR_CHECK(wifi_mqtt_init());
    power_mgmt_init();
    draw_encoder_layout();
    ESP_LOGI(TAG, "LCD encoder display ready");

    int last_raw[ENC_COUNT] = {0};
    int last_acc[ENC_COUNT] = {0};

    while (true) {
        state_led_tick();
        input_map_tick();

        bool user_active = false;
        for (int i = 0; i < ENC_COUNT; i++) {
            int raw;
            int acc;
            enc_get(i, &raw, &acc);

            if (raw != last_raw[i]) {
                st7789_draw_label_number("RAW ", raw, TEXT_X, s_enc_views[i].y_raw,
                                         COLOR_RAW, COLOR_BG, TEXT_SCALE, RAW_MAX_CHARS);
                last_raw[i] = raw;
                user_active = true;
            }
            if (acc != last_acc[i]) {
                st7789_draw_label_number("ACC ", acc, TEXT_X, s_enc_views[i].y_acc,
                                         COLOR_ACC, COLOR_BG, TEXT_SCALE, ACC_MAX_CHARS);
                last_acc[i] = acc;
                user_active = true;
            }
        }
        if (user_active) {
            power_mgmt_mark_activity();
        }

        vTaskDelay(pdMS_TO_TICKS(LOOP_DELAY_MS));
    }
}
