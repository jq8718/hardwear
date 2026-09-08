#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_private/io_mux.h"
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

/* Pad pull-up: once rtc_gpio_deinit() has handed an RTC pad (GPIO0-6) to the
 * HP mux (probe: mux_sel=0), the standard HP IO_MUX pull gpio_set_pull_mode()
 * DOES hold it - measured g0 floats 0->1 with HP pull enabled. The LP pull is
 * applied as well so the pad still holds even if it ends up LP-controlled. */
static void encoder_pullup(int gpio)
{
    gpio_set_pull_mode(gpio, GPIO_PULLUP_ONLY);
    if (rtc_gpio_is_valid_gpio(gpio)) {
        ESP_ERROR_CHECK(rtc_gpio_pullup_en(gpio));
    }
}

/* GPIO0-6 can come out of reset held in the LP/RTC pad function, where the
 * digital input path (GPIO matrix / PCNT) never sees the pad even though the
 * pin physically toggles (scope shows quadrature, gpio_get_level stays 0).
 * Observed on the right knob GPIO0/1. rtc_gpio_deinit() hands the pad to the
 * HP digital mux so PCNT counts it, but it ALSO force-disables the LP_IO
 * clock; with that clock gated the LP_IO_MUX pull register drops subsequent
 * rtc_gpio_pullup_en() writes and those pins float low again (the 2026-09-08
 * regression on GPIO4/5). Re-assert the clock for the pad (refcounted, so it
 * stays on) before rtc_gpio_pullup_en() below runs. */
static void encoder_rtc_to_digital(int gpio)
{
    ESP_ERROR_CHECK(rtc_gpio_deinit(gpio));
    io_mux_enable_lp_io_clock(gpio, true);
}

static void encoder_add(int gpio_a, int gpio_b, pcnt_unit_handle_t *ret_unit)
{
    if (rtc_gpio_is_valid_gpio(gpio_a)) {
        encoder_rtc_to_digital(gpio_a);
    }
    if (rtc_gpio_is_valid_gpio(gpio_b)) {
        encoder_rtc_to_digital(gpio_b);
    }

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
    encoder_pullup(gpio_a);
    encoder_pullup(gpio_b);

    // Reject sub-microsecond bounce on the quadrature lines. First line against
    // electrical chatter; slower contact jitter is caught by software detent
    // filtering in input_map, which this window cannot reach.
    pcnt_glitch_filter_config_t glitch = { .max_glitch_ns = 1000 };
    esp_err_t gerr = pcnt_unit_set_glitch_filter(unit, &glitch);
    if (gerr != ESP_OK) {
        ESP_LOGW(TAG, "glitch filter not applied: %s", esp_err_to_name(gerr));
    }

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
    /* Idle level check: with the internal pull alive, any phase the knob is
     * NOT shorting reads HIGH. Left/mid knobs (enc2 GPIO4/5, enc3 GPIO11/12)
     * should read 1/1 at rest; the right knob (enc1 GPIO0/1) parks with both
     * phases closed, so 0/0 there is expected. A floating 0 on a pad the knob
     * leaves open means the pull was lost (LP_IO clock gated on RTC pads). */
    ESP_LOGI(TAG, "enc idle lv: enc1 %d/%d  enc2 %d/%d  enc3 %d/%d",
             gpio_get_level(ENC1_GPIO_A), gpio_get_level(ENC1_GPIO_B),
             gpio_get_level(ENC2_GPIO_A), gpio_get_level(ENC2_GPIO_B),
             gpio_get_level(ENC3_GPIO_A), gpio_get_level(ENC3_GPIO_B));
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
