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
#include "lcd_vt.h"
#include "power_mgmt.h"
#include "state_led.h"
#include "st7789.h"
#include "wifi_mqtt.h"
#include "nvs_config.h"

#define ENC_COUNT 3
#define LOOP_DELAY_MS 20

static const char *TAG = "main";

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
    lcd_vt_init();
    ESP_LOGI(TAG, "LCD status/terminal view ready");

    int last_raw[ENC_COUNT] = {0};
    int last_acc[ENC_COUNT] = {0};

    while (true) {
        state_led_tick();
        input_map_tick();
        lcd_vt_poll();

        bool user_active = false;
        for (int i = 0; i < ENC_COUNT; i++) {
            int raw;
            int acc;
            enc_get(i, &raw, &acc);
            if (raw != last_raw[i] || acc != last_acc[i]) {
                last_raw[i] = raw;
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
