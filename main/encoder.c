#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "hal/pcnt_ll.h"

#include "encoder.h"

#define ENC1_GPIO_A 0
#define ENC1_GPIO_B 1
#define ENC2_GPIO_A 4
#define ENC2_GPIO_B 5
#define ENC3_GPIO_A 11
#define ENC3_GPIO_B 12

#define ENC_LOW_LIMIT  -32768
#define ENC_HIGH_LIMIT  32767

// ESP32-C5 has a single PCNT group and 4 units. encoder_init() allocates them
// in order (encoder 1 first, encoder 2 second, encoder 3 third), so they land
// on units 0, 1, and 2.
#define ENC1_UNIT_ID 0
#define ENC2_UNIT_ID 1
#define ENC3_UNIT_ID 2

static const char *TAG = "encoder";

static pcnt_unit_handle_t s_enc1_unit = NULL;
static pcnt_unit_handle_t s_enc2_unit = NULL;
static pcnt_unit_handle_t s_enc3_unit = NULL;

static void encoder_add(int gpio_a, int gpio_b, pcnt_unit_handle_t *ret_unit)
{
    pcnt_unit_handle_t unit = NULL;
    pcnt_unit_config_t unit_config = {
        .low_limit = ENC_LOW_LIMIT,
        .high_limit = ENC_HIGH_LIMIT,
        .flags.accum_count = 1,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, &unit));

    // Full quadrature decoding uses two channels on one unit (A/B swapped).
    pcnt_chan_config_t chan_a_config = {
        .edge_gpio_num = gpio_a,
        .level_gpio_num = gpio_b,
    };
    pcnt_channel_handle_t chan_a = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(unit, &chan_a_config, &chan_a));

    pcnt_chan_config_t chan_b_config = {
        .edge_gpio_num = gpio_b,
        .level_gpio_num = gpio_a,
    };
    pcnt_channel_handle_t chan_b = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(unit, &chan_b_config, &chan_b));

    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan_a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chan_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan_b, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chan_b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    // The PCNT driver only routes the signals; enable the internal pull-ups here.
    gpio_set_pull_mode(gpio_a, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(gpio_b, GPIO_PULLUP_ONLY);

    ESP_ERROR_CHECK(pcnt_unit_enable(unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(unit));
    ESP_ERROR_CHECK(pcnt_unit_start(unit));

    *ret_unit = unit;
}

esp_err_t encoder_init(void)
{
    encoder_add(ENC1_GPIO_A, ENC1_GPIO_B, &s_enc1_unit);
    encoder_add(ENC2_GPIO_A, ENC2_GPIO_B, &s_enc2_unit);
    encoder_add(ENC3_GPIO_A, ENC3_GPIO_B, &s_enc3_unit);
    ESP_LOGI(TAG, "encoders ready: enc1 GPIO%d/%d, enc2 GPIO%d/%d, enc3 GPIO%d/%d",
             ENC1_GPIO_A, ENC1_GPIO_B, ENC2_GPIO_A, ENC2_GPIO_B, ENC3_GPIO_A, ENC3_GPIO_B);
    return ESP_OK;
}

int encoder1_get_count(void)
{
    int value = 0;
    pcnt_unit_get_count(s_enc1_unit, &value);
    return value;
}

int encoder2_get_count(void)
{
    int value = 0;
    pcnt_unit_get_count(s_enc2_unit, &value);
    return value;
}

int encoder1_get_raw(void)
{
    return pcnt_ll_get_count(PCNT_LL_GET_HW(0), ENC1_UNIT_ID);
}

int encoder2_get_raw(void)
{
    return pcnt_ll_get_count(PCNT_LL_GET_HW(0), ENC2_UNIT_ID);
}

int encoder3_get_count(void)
{
    int value = 0;
    pcnt_unit_get_count(s_enc3_unit, &value);
    return value;
}

int encoder3_get_raw(void)
{
    return pcnt_ll_get_count(PCNT_LL_GET_HW(0), ENC3_UNIT_ID);
}
