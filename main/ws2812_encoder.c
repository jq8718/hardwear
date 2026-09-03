#include "esp_check.h"
#include "ws2812_encoder.h"

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} ws2812_encoder_t;

RMT_ENCODER_FUNC_ATTR
static size_t ws2812_encode(rmt_encoder_t *encoder,
                            rmt_channel_handle_t channel,
                            const void *data,
                            size_t data_size,
                            rmt_encode_state_t *ret_state)
{
    ws2812_encoder_t *ws_encoder = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    if (ws_encoder->state == 0) {
        encoded_symbols += ws_encoder->bytes_encoder->encode(
            ws_encoder->bytes_encoder, channel, data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            ws_encoder->state = 1;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto done;
        }
    }

    if (ws_encoder->state == 1) {
        encoded_symbols += ws_encoder->copy_encoder->encode(
            ws_encoder->copy_encoder, channel, &ws_encoder->reset_code,
            sizeof(ws_encoder->reset_code), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            ws_encoder->state = 0;
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
        }
    }

done:
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t ws2812_encoder_delete(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *ws_encoder = __containerof(encoder, ws2812_encoder_t, base);
    rmt_del_encoder(ws_encoder->bytes_encoder);
    rmt_del_encoder(ws_encoder->copy_encoder);
    free(ws_encoder);
    return ESP_OK;
}

RMT_ENCODER_FUNC_ATTR
static esp_err_t ws2812_encoder_reset(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *ws_encoder = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encoder_reset(ws_encoder->bytes_encoder);
    rmt_encoder_reset(ws_encoder->copy_encoder);
    ws_encoder->state = 0;
    return ESP_OK;
}

esp_err_t ws2812_encoder_new(const ws2812_encoder_config_t *config,
                             rmt_encoder_handle_t *ret_encoder)
{
    esp_err_t ret = ESP_OK;
    ws2812_encoder_t *ws_encoder = NULL;

    ESP_GOTO_ON_FALSE(config && ret_encoder, ESP_ERR_INVALID_ARG, error,
                      "ws2812", "invalid encoder configuration");
    ws_encoder = rmt_alloc_encoder_mem(sizeof(ws2812_encoder_t));
    ESP_GOTO_ON_FALSE(ws_encoder, ESP_ERR_NO_MEM, error,
                      "ws2812", "encoder allocation failed");

    ws_encoder->base.encode = ws2812_encode;
    ws_encoder->base.del = ws2812_encoder_delete;
    ws_encoder->base.reset = ws2812_encoder_reset;

    rmt_bytes_encoder_config_t bytes_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = 0.3 * config->resolution / 1000000,
            .level1 = 0,
            .duration1 = 0.9 * config->resolution / 1000000,
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = 0.9 * config->resolution / 1000000,
            .level1 = 0,
            .duration1 = 0.3 * config->resolution / 1000000,
        },
        .flags.msb_first = 1,
    };
    ESP_GOTO_ON_ERROR(rmt_new_bytes_encoder(&bytes_config,
                                            &ws_encoder->bytes_encoder),
                         error, "ws2812", "byte encoder creation failed");

    rmt_copy_encoder_config_t copy_config = {};
    ESP_GOTO_ON_ERROR(rmt_new_copy_encoder(&copy_config,
                                           &ws_encoder->copy_encoder),
                         error, "ws2812", "reset encoder creation failed");

    uint32_t reset_ticks = config->resolution / 1000000 * 50 / 2;
    ws_encoder->reset_code = (rmt_symbol_word_t) {
        .level0 = 0,
        .duration0 = reset_ticks,
        .level1 = 0,
        .duration1 = reset_ticks,
    };
    *ret_encoder = &ws_encoder->base;
    return ESP_OK;

error:
    if (ws_encoder) {
        if (ws_encoder->bytes_encoder) {
            rmt_del_encoder(ws_encoder->bytes_encoder);
        }
        if (ws_encoder->copy_encoder) {
            rmt_del_encoder(ws_encoder->copy_encoder);
        }
        free(ws_encoder);
    }
    return ret;
}
