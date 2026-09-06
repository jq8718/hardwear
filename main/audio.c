#include <stdbool.h>
#include <string.h>

#include "driver/i2s_pdm.h"
#include "esp_log.h"
#include "math.h"

#include "audio.h"

#define PDM_BCLK_GPIO   24
#define PDM_DIN_GPIO    23   /* RX: dual-mic data in (L/R hardwired) */

static const char *TAG = "audio";

/* ESP32-C5 I2S PDM RX is raw-only (no hardware PDM2PCM filter), so the line
 * rate is the PDM oversample rate (~MHz); software downsamples to 16 kHz PCM. */
#define RX_RAW_PDM_FREQ_HZ  2048000

/* Software CIC2 decimation: ~2.048MHz raw PDM -> 16kHz. The on-wire PDM clock
 * can differ from 2.048MHz by a small ratio once a real mic is attached; this
 * single constant is the bring-up tuning point. */
#define PDM_DECIM_M     128
#define CIC_INT64_LIMIT (1LL << 42)

typedef struct {
    int64_t i1, i2;        /* order-2 CIC integrators (run at input rate)  */
    int64_t s_prev, e1_prev; /* difference stages (run at output rate)      */
    uint32_t n;
} cic2_t;

static int cic2_push(cic2_t *c, int bit, int16_t *out)
{
    int64_t x = bit ? 1 : -1;
    c->i1 += x;
    c->i2 += c->i1;
    if (c->i1 > CIC_INT64_LIMIT || c->i1 < -CIC_INT64_LIMIT ||
        c->i2 > CIC_INT64_LIMIT || c->i2 < -CIC_INT64_LIMIT) {
        c->i1 = 0;   /* pathological near-DC input guard; real PDM is whitened */
        c->i2 = 0;
    }
    if (++c->n >= PDM_DECIM_M) {
        c->n = 0;
        int64_t s   = c->i2;
        int64_t e1  = s - c->s_prev;  c->s_prev = s;
        int64_t e2  = e1 - c->e1_prev; c->e1_prev = e1;
        int64_t sc  = (e2 * 32767) / ((int64_t)PDM_DECIM_M * PDM_DECIM_M);
        if (sc > 32767) sc = 32767;
        if (sc < -32767) sc = -32767;
        *out = (int16_t) sc;
        return 1;
    }
    return 0;
}

/* Decimate a run of raw 16-bit PDM slots (bit = slot MSB) into 16 kHz mono.
 * Returns number of 16 kHz samples produced (<= out_cap). Caller keeps feeding
 * consecutive buffers; decoder state lives in *st across calls. */
esp_err_t audio_pdm_decimate(const int16_t *raw, size_t slots, int16_t *out,
                             size_t out_cap, size_t *out_n, void *st)
{
    cic2_t *c = st;
    size_t n = 0;
    for (size_t i = 0; i < slots; i++) {
        int bit = (raw[i] >> 15) & 1;
        int16_t s;
        if (cic2_push(c, bit, &s)) {
            if (n < out_cap) {
                out[n++] = s;
            }
        }
    }
    if (out_n) {
        *out_n = n;
    }
    return ESP_OK;
}

size_t audio_pdm_decoder_size(void) { return sizeof(cic2_t); }

/* Self-test: synthesize a 440 Hz sine as 1-bit PDM (1st-order sigma-delta at
 * 2.048MHz, integer math only -- C5 has no FPU), decimate, and check that the
 * recovered dominant frequency is ~440 Hz. */
static void audio_decim_selftest(void)
{
    static cic2_t st_cic;
    cic2_t *c = &st_cic;
    memset(c, 0, sizeof(*c));

    const int kPdm = 2048000, kFs = 16000;   /* PDM clock and PCM rate        */
    const int kSlot = kPdm / 10;             /* 0.1 s of PDM slots = 1600 pcm */
    const int kTab = 512;
    static int16_t stab[512];
    static int16_t pcm[4096];              /* static: main-task stack is 3.5 KB */

    for (int i = 0; i < kTab; i++) {
        stab[i] = (int16_t)(12000 * sinf(2.0f * (float) M_PI * i / kTab));
    }
    size_t n = 0;
    uint32_t spp = 0;
    const uint32_t spstep = (uint32_t)(((uint64_t) 440 << 16) / (uint64_t) kPdm);
    int32_t acc = 0;
    for (int k = 0; k < kSlot; k++) {
        int16_t ref = stab[(spp >> 7) & (kTab - 1)];   /* Q16 over 512-table: /128 */
        spp += spstep;
        int bit = acc >= 0 ? 1 : 0;
        acc += ref + (bit ? -32768 : 32768);
        int16_t out;
        if (cic2_push(c, bit, &out)) {
            if (n < 4096) {
                pcm[n++] = out;
            }
        }
    }

    /* Zero-crossing rate over the middle 90% of the output gives the freq. */
    size_t zc = 0, base = n / 20;
    for (size_t i = base + 1; i < n - base; i++) {
        if ((pcm[i - 1] < 0 && pcm[i] >= 0) || (pcm[i - 1] >= 0 && pcm[i] < 0)) {
            zc++;
        }
    }
    float secs = (float) (n - 2 * base) / (float) kFs;
    float fmeas = secs > 0 ? (float) zc / (2.0f * secs) : 0.0f;
    bool ok = fmeas > 340.0f && fmeas < 540.0f;
    ESP_LOGI(TAG, "decimator self-test: %u zc over %.3fs -> %.0f Hz (expect ~440) %s",
             (unsigned) zc, secs, fmeas, ok ? "OK" : "FAIL");
}

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

    audio_decim_selftest();

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
