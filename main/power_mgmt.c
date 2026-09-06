#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ota_mqtt.h"
#include "power_mgmt.h"

#define TAG "power"

/* Idle timeout before deep-sleep. Bring-up builds drop this to ~12 s to
 * observe sleep/wake quickly; 600 s keeps the device awake through normal
 * desk use and only naps after a long quiet stretch. */
#ifndef POWER_IDLE_SEC
#define POWER_IDLE_SEC 600
#endif

/* Periodic wake after sleep: the ESP32-C5's EXT1 is level-based, and this
 * unit's encoder quadrature lines can rest low at a detent, which would either
 * re-wake instantly (EXT1 armed) or strand the chip un-wakeable (EXT1 skipped).
 * The timer is therefore always armed as a reliable net so the device
 * reconnects MQTT and pulls fresh status; EXT1 additionally wakes on a knob
 * turn whenever the lines rest high. 0 disables the timer. */
#ifndef POWER_WAKE_TIMER_SEC
#define POWER_WAKE_TIMER_SEC 60
#endif

/* EXT1 wake pins: only the ESP32-C5's RTC-capable GPIO0..6 can wake it from
 * deep sleep. ENC1 (GPIO0/1) and ENC2 (GPIO4/5) lines sit high at rest (they
 * are pulled up as PCNT inputs) and a rotation drags one low -> ANY_LOW wake.
 * GPIO28 (PCA9535 INT) and encoder3 GPIO11/12 are NOT RTC GPIOs and cannot
 * wake the chip; deep-sleep wake on knob turn is therefore ENC1/ENC2 only. */
#define WAKE_PINS_MSK ((1ULL << 0) | (1ULL << 1) | (1ULL << 4) | (1ULL << 5))

#define POLL_MS 1000

static volatile TickType_t s_last_activity;

void power_mgmt_mark_activity(void)
{
    s_last_activity = xTaskGetTickCount();
}

static bool wake_pins_high(void)
{
    const gpio_num_t pins[] = { GPIO_NUM_0, GPIO_NUM_1, GPIO_NUM_4, GPIO_NUM_5 };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        if (gpio_get_level(pins[i]) == 0) {
            return false;
        }
    }
    return true;
}

static void log_wake_pin_levels(void)
{
    const gpio_num_t pins[] = { GPIO_NUM_0, GPIO_NUM_1, GPIO_NUM_4, GPIO_NUM_5 };
    const char *names[] = { "ENC1A(gp0)", "ENC1B(gp1)", "ENC2A(gp4)", "ENC2B(gp5)" };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        ESP_LOGI(TAG, "  %s=%d", names[i], (int) gpio_get_level(pins[i]));
    }
}

static void sleep_now(void)
{
    const bool high = wake_pins_high();
    /* EXT1 ANY_LOW is level-based: if a quadrature channel happens to rest low
     * at the knob's current detent, arming it would re-wake the chip instantly.
     * Only arm EXT1 when every wake pin reads high (a genuine turn then drags
     * a line low). Otherwise sleep on the periodic timer alone so a low-parked
     * knob does not defeat deep sleep; that just delays knob-wake to the next
     * timer period. */
    if (!high) {
        ESP_LOGI(TAG, "encoder pin low at detent: timer-only wake this cycle");
        log_wake_pin_levels();
    }
    ESP_LOGI(TAG, "idle %d s -> deep sleep, wake %s",
             POWER_IDLE_SEC, high ? "ENC1/ENC2 GPIO or timer" : "timer");
    if (high) {
        /* esp_deep_sleep_start pulls the wake lines per mode (ANY_LOW ->
         * pull-up) automatically, so no rtc_gpio_pullup_en() calls. */
        esp_sleep_enable_ext1_wakeup_io(WAKE_PINS_MSK, ESP_EXT1_WAKEUP_ANY_LOW);
    }
#if POWER_WAKE_TIMER_SEC > 0
    esp_sleep_enable_timer_wakeup((uint64_t) POWER_WAKE_TIMER_SEC * 1000000ULL);
#endif
    esp_deep_sleep_start(); /* noreturn: wake reboots into app_main */
}

void power_mgmt_sleep_now(void)
{
    sleep_now();
}

static void power_task(void *arg)
{
    const TickType_t idle_ticks = pdMS_TO_TICKS(POWER_IDLE_SEC * 1000u);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        /* Never cut an OTA download short. */
        if (ota_mqtt_busy()) {
            power_mgmt_mark_activity();
            continue;
        }
        TickType_t idle = xTaskGetTickCount() - s_last_activity; /* wraps safe */
        if (idle >= idle_ticks) {
            sleep_now();
        }
    }
}

void power_mgmt_init(void)
{
    power_mgmt_mark_activity();
    if (xTaskCreate(power_task, "power", 2048, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "power task create failed");
    } else {
#if POWER_WAKE_TIMER_SEC > 0
        ESP_LOGI(TAG, "idle deep-sleep armed (%d s, wake EXT1 GPIO0/1/4/5 or timer)",
                 POWER_IDLE_SEC);
#else
        ESP_LOGI(TAG, "idle deep-sleep armed (%d s, wake EXT1 GPIO0/1/4/5)",
                 POWER_IDLE_SEC);
#endif
    }
}
