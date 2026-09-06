#include "driver/i2s_pdm.h"
#include "esp_log.h"

#include "audio.h"

#define PDM_BCLK_GPIO   24
#define PDM_DIN_GPIO    23   /* RX: dual-mic data in (L/R hardwired) */

/* ESP32-C5 I2S PDM RX is raw-only (no hardware PDM2PCM filter), so the line
 * rate is the PDM oversample rate (~MHz); software downsamples to 16 kHz PCM. */
#define RX_RAW_PDM_FREQ_HZ  2048000

static const char *TAG = "audio";

static i2s_chan_handle_t s_rx_chan = NULL;

esp_err_t audio_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan));

    i2s_pdm_rx_config_t rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(RX_RAW_PDM_FREQ_HZ),
        .slot_cfg = I2S_PDM_RX_SLOT_RAW_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .clk = PDM_BCLK_GPIO,
            .din = PDM_DIN_GPIO,
            .invert_flags = { .clk_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(s_rx_chan, &rx_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));

    ESP_LOGI(TAG, "PDM RX ready: raw %.2fMHz stereo (clk=%d din=%d)",
             RX_RAW_PDM_FREQ_HZ / 1000000.0f, PDM_BCLK_GPIO, PDM_DIN_GPIO);

    // Quick RX self-test: no MIC connected -> silence or timeout, must not crash.
    int16_t probe[64];
    size_t got = 0;
    esp_err_t read_err = i2s_channel_read(s_rx_chan, probe, sizeof(probe), &got, 100);
    ESP_LOGI(TAG, "RX self-test: %s, %d bytes read", esp_err_to_name(read_err), (int) got);

    return ESP_OK;
}

esp_err_t audio_capture_read(int16_t *buf, size_t samples, size_t *samples_read)
{
    size_t bytes = 0;
    esp_err_t err = i2s_channel_read(s_rx_chan, buf, samples * sizeof(int16_t), &bytes, 100);
    if (samples_read) {
        *samples_read = bytes / sizeof(int16_t);
    }
    return err;
}
