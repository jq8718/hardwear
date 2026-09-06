#include <stdlib.h>

#include "esp_log.h"

#include "encoder.h"
#include "input_map.h"
#include "wifi_mqtt.h"

static const char *TAG = "input_map";

static int s_last_enc1 = 0;
static int s_last_enc2 = 0;
static int s_last_enc3 = 0;

static void send_arrow(const uint8_t *seq, int count)
{
    int n = abs(count);
    if (n > 3) {
        n = 3;   /* cap per-tick burst */
    }
    for (int i = 0; i < n; i++) {
        wifi_mqtt_send_key(seq, 3);
    }
}

esp_err_t input_map_init(void)
{
    s_last_enc1 = encoder1_get_count();
    s_last_enc2 = encoder2_get_count();
    s_last_enc3 = encoder3_get_count();
    ESP_LOGI(TAG, "input map ready");
    return ESP_OK;
}

void input_map_tick(void)
{
    int c1 = encoder1_get_count();
    int c2 = encoder2_get_count();
    int c3 = encoder3_get_count();

    int d1 = c1 - s_last_enc1;
    int d2 = c2 - s_last_enc2;
    int d3 = c3 - s_last_enc3;

    s_last_enc1 = c1;
    s_last_enc2 = c2;
    s_last_enc3 = c3;

    // Encoder1: vertical scroll (arrow up/down).
    if (d1 != 0) {
        static const uint8_t down[3] = {0x1b, '[', 'B'};
        static const uint8_t up[3]   = {0x1b, '[', 'A'};
        send_arrow(d1 > 0 ? down : up, d1);
        ESP_LOGI(TAG, "enc1 delta %d -> %s", d1, d1 > 0 ? "down" : "up");
    }

    // Encoder2: horizontal / PageUp-PageDown (use left/right for now).
    if (d2 != 0) {
        static const uint8_t right[3] = {0x1b, '[', 'C'};
        static const uint8_t left[3]  = {0x1b, '[', 'D'};
        send_arrow(d2 > 0 ? right : left, d2);
        ESP_LOGI(TAG, "enc2 delta %d -> %s", d2, d2 > 0 ? "right" : "left");
    }

    // Encoder3: reserved (instance switch / backlight), not sent over MQTT yet.
    if (d3 != 0) {
        ESP_LOGI(TAG, "enc3 delta %d (unmapped)", d3);
    }
}
