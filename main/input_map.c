#include <stdlib.h>

#include "esp_log.h"

#include "encoder.h"
#include "input_map.h"
#include "wifi_mqtt.h"

static const char *TAG = "input_map";

/* A mechanical detent = 4 PCNT counts (hardware 4x quadrature). Sub-detent
 * contact jitter (isolated ±1 oscillation) must never reach the wire: that was
 * the GPIO4/5 idle chatter turning into phantom mouse-wheel scrolls. */
#define DETENT 4

/* ESC payloads published to <prefix>/pty_in. The mirror relay interprets:
 *   ESC[A / ESC[B -> LCD scrollback (back into history / toward live)
 *   ESC[C / ESC[D -> synthetic mouse wheel (down / up)
 *
 * Channel->role assignment (channel = encoder index into s_count below:
 * 0 = GPIO0/1 enc1, 1 = GPIO4/5 enc2, 2 = GPIO11/12 enc3). Bench probe
 * 2026-09-06 showed the physical LEFT knob (LCD history) reads on GPIO4/5 and
 * the MIDDLE knob (wheel) on GPIO11/12; GPIO0/1 is the unused RIGHT knob.
 * Re-point a role by editing the index and reflashing. */
#define CH_LCD   1     /* sends ESC[A/B : LCD scrollback */
#define CH_WHEEL 2     /* sends ESC[C/D : synthetic wheel */
#define CH_SPARE 0     /* unmapped */

static const uint8_t ESC_A[3] = {0x1b, '[', 'A'};
static const uint8_t ESC_B[3] = {0x1b, '[', 'B'};
static const uint8_t ESC_C[3] = {0x1b, '[', 'C'};
static const uint8_t ESC_D[3] = {0x1b, '[', 'D'};

typedef int (*count_fn)(void);
static count_fn s_count[3] = {
    encoder1_get_count,
    encoder2_get_count,
    encoder3_get_count,
};

static int s_last[3] = {0, 0, 0};
static int s_acc[3]  = {0, 0, 0};   /* sub-detent remainder, keeps sign */

esp_err_t input_map_init(void)
{
    for (int i = 0; i < 3; i++) {
        s_last[i] = s_count[i]();
        s_acc[i] = 0;
    }
    ESP_LOGI(TAG, "input map ready: LCD=ch%d wheel=ch%d spare=ch%d (detent=%d)",
             CH_LCD, CH_WHEEL, CH_SPARE, DETENT);
    return ESP_OK;
}

static void fire(int ch, int det)
{
    const uint8_t *seq = (ch == CH_LCD) ? (det < 0 ? ESC_A : ESC_B)
                                        : (det < 0 ? ESC_D : ESC_C);
    int n = abs(det);
    if (n > 4) {
        n = 4;   /* pace each tick; unused detents stay banked in s_acc */
    }
    for (int i = 0; i < n; i++) {
        wifi_mqtt_send_key(seq, 3);
    }
    ESP_LOGI(TAG, "ch%d %s x%d", ch, det < 0 ? "ccw" : "cw", n);
}

void input_map_tick(void)
{
    for (int ch = 0; ch < 3; ch++) {
        int c = s_count[ch]();
        int d = c - s_last[ch];
        s_last[ch] = c;
        if (d == 0) {
            continue;
        }
        /* Debounce by plain signed accumulation, no reversal reset. Idle
         * contact jitter alternates ±1 so it can never reach a full detent,
         * while a real (jittery) slow turn still accumulates monotonically and
         * fires every detent. A reversal-reset here dropped genuine slow turns
         * on the left knob, which is exactly the channel that jitters. */
        s_acc[ch] += d;
        int det = s_acc[ch] / DETENT;
        if (det == 0) {
            continue;   /* sub-detent jitter: hold, do not emit */
        }
        /* Fire whole detents; clamp what this tick sends so a burst drains
         * smoothly, and subtract exactly the fired amount so nothing is lost. */
        int fired = det > 0 ? (det > 4 ? 4 : det) : (det < -4 ? -4 : det);
        s_acc[ch] -= fired * DETENT;
        if (ch == CH_SPARE) {
            ESP_LOGI(TAG, "spare ch%d x%d (unmapped)", ch, abs(fired));
        } else {
            fire(ch, fired);
        }
    }
}
