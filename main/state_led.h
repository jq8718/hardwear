#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_STATE_OFFLINE = 0,   /* red blink: disconnected */
    LED_STATE_CONNECTING,    /* orange pulse: wifi/mqtt connecting */
    LED_STATE_WAITING,       /* green breathe: agent waiting */
    LED_STATE_WORKING,       /* blue pulse: agent working */
    LED_STATE_YOLO,          /* purple pulse: auto/YOLO mode */
    LED_STATE_COUNT,
} led_state_t;

esp_err_t state_led_init(void);
void state_led_set(led_state_t state);
void state_led_tick(void);

#ifdef __cplusplus
}
#endif
