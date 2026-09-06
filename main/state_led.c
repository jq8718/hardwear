#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "ws2812_encoder.h"

#include "state_led.h"

#define WS2812_GPIO              27
#define WS2812_RMT_RESOLUTION_HZ 10000000
#define LED_MAX                  32   /* peak brightness, keep low for single LED */

typedef enum {
    ANIM_SOLID = 0,
    ANIM_BREATHE,   /* slow triangle wave */
    ANIM_PULSE,     /* faster triangle wave */
    ANIM_BLINK,     /* square on/off */
} led_anim_t;

typedef struct {
    uint8_t r, g, b;      /* base color at peak brightness */
    led_anim_t anim;
    uint32_t period_ms;
} led_state_def_t;

static const led_state_def_t s_states[LED_STATE_COUNT] = {
    [LED_STATE_OFFLINE]    = { LED_MAX, 0,        0,        ANIM_BLINK,   600  },
    [LED_STATE_CONNECTING] = { LED_MAX, 20,       0,        ANIM_PULSE,   800  },
    [LED_STATE_WAITING]    = { 0,       LED_MAX,  0,        ANIM_BREATHE, 2000 },
    [LED_STATE_WORKING]    = { 0,       0,        LED_MAX,  ANIM_PULSE,   600  },
    [LED_STATE_YOLO]       = { LED_MAX, 0,        LED_MAX,  ANIM_PULSE,   1000 },
};

static const char *TAG = "state_led";

static led_state_t s_state = LED_STATE_OFFLINE;
static rmt_channel_handle_t s_channel = NULL;
static rmt_encoder_handle_t s_encoder = NULL;

esp_err_t state_led_init(void)
{
    rmt_tx_channel_config_t ch_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = WS2812_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = WS2812_RMT_RESOLUTION_HZ,
        .trans_queue_depth = 1,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&ch_cfg, &s_channel));

    ws2812_encoder_config_t enc_cfg = {
        .resolution = WS2812_RMT_RESOLUTION_HZ,
    };
    ESP_ERROR_CHECK(ws2812_encoder_new(&enc_cfg, &s_encoder));
    ESP_ERROR_CHECK(rmt_enable(s_channel));

    ESP_LOGI(TAG, "status LED ready on GPIO%d", WS2812_GPIO);
    return ESP_OK;
}

void state_led_set(led_state_t state)
{
    if (state >= LED_STATE_COUNT) {
        state = LED_STATE_OFFLINE;
    }
    s_state = state;
}

static uint8_t anim_factor(led_anim_t anim, uint32_t period_ms)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t phase = now % period_ms;

    switch (anim) {
    case ANIM_SOLID:
        return 255;
    case ANIM_BLINK:
        return (phase < period_ms / 2) ? 255 : 0;
    case ANIM_BREATHE:
    case ANIM_PULSE:
    default: {
        uint32_t half = period_ms / 2;
        uint32_t x = (phase < half) ? phase : (period_ms - phase);
        return (uint8_t)(x * 255 / half);
    }
    }
}

void state_led_tick(void)
{
    const led_state_def_t *def = &s_states[s_state];
    uint8_t f = anim_factor(def->anim, def->period_ms);

    // WS2812 expects GRB byte order.
    uint8_t grb[3] = {
        (uint8_t)(def->g * f / 255),
        (uint8_t)(def->r * f / 255),
        (uint8_t)(def->b * f / 255),
    };

    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    ESP_ERROR_CHECK(rmt_transmit(s_channel, s_encoder, grb, sizeof(grb), &tx_cfg));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(s_channel, portMAX_DELAY));
}
