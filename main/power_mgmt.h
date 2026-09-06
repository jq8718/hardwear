#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Idle low-power (Stage 8c): deep-sleep the chip after a configurable idle
 * period with no user activity, waking on any of the encoder GPIOs that are
 * RTC-capable on the ESP32-C5 (GPIO0/1/4/5). A wake restarts the app fresh,
 * so normal boot handles reconnect. */
void power_mgmt_init(void);

/* Called whenever the user produces input (encoder rotation, key delivery). */
void power_mgmt_mark_activity(void);

/* Log-and-sleep is only ever done by the internal monitor task. */
void power_mgmt_sleep_now(void);

#ifdef __cplusplus
}
#endif
