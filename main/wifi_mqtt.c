#include <string.h>
#include <stdio.h>

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"

#include "wifi_mqtt.h"
#include "state_led.h"
#include "nvs_config.h"
#include "ota_mqtt.h"
#include "lcd_vt.h"
#include "screen_jpeg.h"
#include "st7789.h"

/* WiFi / broker credentials and target instance now come from NVS
 * (nvs_config), so a reflash no longer hard-codes the network. */
#define DISCOVERY_TOPIC  "+/+/+/vibetty"

/* While no instance is locked, only adopt a presence whose user segment is
 * BIND_USER. A shared broker (broker.emqx.io) carries foreign tenants under
 * their own user namespace (e.g. "vibekeys"); without this gate the first
 * random retained presence could hijack binding before our real instance
 * appears. The value mirrors vibetty's [mqtt] username fallback ("root"). */
#define BIND_USER  "root"

static const char *TAG = "wifi_mqtt";

static esp_mqtt_client_handle_t s_mqtt = NULL;
static bool s_mqtt_connected = false;
static bool s_has_instance = false;
static char s_prefix[128] = {0};

static bool ends_with(const char *s, const char *suffix)
{
    size_t sl = strlen(s);
    size_t xl = strlen(suffix);
    return sl >= xl && strcmp(s + sl - xl, suffix) == 0;
}

static void handle_screen_text(const uint8_t *data, int len)
{
    if (len < 2) {
        return;
    }
    uint8_t tag = data[0];
    int text_len = len - 1;
    /* Feed the ANSI terminal. These run on the MQTT task and only mutate the
     * char grid + dirty-row map; actual SPI drawing happens in lcd_vt_poll on
     * the app task, so the display stays single-threaded. */
    if (tag == 0x00) {
        ESP_LOGI(TAG, "screen_text baseline %d bytes", text_len);
        lcd_vt_feed_baseline(data + 1, text_len);
    } else if (tag == 0x01) {
        lcd_vt_feed(data + 1, text_len);
    } else {
        ESP_LOGW(TAG, "screen_text unknown tag %d", tag);
    }
}

static bool first_seg_eq(const char *p, const char *seg)
{
    size_t sl = strlen(seg);
    return strncmp(p, seg, sl) == 0 && (p[sl] == '/' || p[sl] == 0);
}

/* vibetty prefix = {user}/{device}/{pid}/vibetty. pid differs on every run,
 * so match the stable user/device pair, never the exact string. */
static bool same_user_device(const char *a, const char *b)
{
    const char *a2 = strchr(a, '/');
    const char *b2 = strchr(b, '/');
    if (!a2 || !b2 || a2 - a != b2 - b || strncmp(a, b, (size_t)(a2 - a)) != 0) {
        return false;
    }
    const char *a3 = strchr(a2 + 1, '/');
    const char *b3 = strchr(b2 + 1, '/');
    if (!a3 || !b3 || a3 - a2 != b3 - b2 || strncmp(a2, b2, (size_t)(a3 - a2)) != 0) {
        return false;
    }
    return true;
}

static void handle_presence(const char *topic, const char *data, int len)
{
    (void) topic;
    if (len == 0) {
        s_has_instance = false;
        ESP_LOGW(TAG, "instance offline (LWT)");
        state_led_set(LED_STATE_OFFLINE);
        lcd_vt_set_pixels(false);
        screen_jpeg_reset();
        lcd_vt_set_status(LCD_ST_OFFLINE, NULL);
        return;
    }

    cJSON *root = cJSON_ParseWithLength(data, len);
    if (root == NULL) {
        return;
    }
    cJSON *prefix = cJSON_GetObjectItem(root, "prefix");
    cJSON *format = cJSON_GetObjectItem(root, "format");
    cJSON *state  = cJSON_GetObjectItem(root, "state");
    cJSON *title  = cJSON_GetObjectItem(root, "title");

    if (cJSON_IsString(prefix) && cJSON_IsString(format)) {
        const char *target = nvs_config_target_prefix();
        if (target[0]) {
            /* Locked instance: match by user/device so the same vibetty on a
             * new pid (fresh run) re-binds without an NVS update. */
            if (!same_user_device(target, prefix->valuestring)) {
                ESP_LOGI(TAG, "presence prefix=%s not our target %s, ignore",
                         prefix->valuestring, target);
                cJSON_Delete(root);
                return;
            }
        } else if (!first_seg_eq(prefix->valuestring, BIND_USER)) {
            ESP_LOGI(TAG, "presence prefix=%s not under user %s, ignore",
                     prefix->valuestring, BIND_USER);
            cJSON_Delete(root);
            return;
        }

        strncpy(s_prefix, prefix->valuestring, sizeof(s_prefix) - 1);
        s_has_instance = true;
        if (!target[0]) {
            /* First adoption: lock the stable user/device pair in NVS so later
             * random presences cannot hijack s_prefix (multi-instance safety). */
            nvs_config_save_target_prefix(s_prefix);
        }

        bool text_mode = strcmp(format->valuestring, "text") == 0;
        bool pixels = !text_mode;   /* "high"/"medium"/"low" = JPEG screen */
        char topic_buf[160];
        snprintf(topic_buf, sizeof(topic_buf), "%s/%s", s_prefix,
                 text_mode ? "screen_text" : "screen");
        esp_mqtt_client_subscribe(s_mqtt, topic_buf, 0);

        ESP_LOGI(TAG, "instance prefix=%s format=%s pixels=%d title=%s",
                 s_prefix, format->valuestring, pixels ? 1 : 0,
                 cJSON_IsString(title) ? title->valuestring : "?");

        /* A JPEG-capable instance is asked to rasterize exactly the canvas
         * body (below the status header) so decoded pixels map 1:1. A text
         * instance still gets the old char-grid PTY size. */
        lcd_vt_set_pixels(pixels);
        char sync_buf[128];
        snprintf(sync_buf, sizeof(sync_buf),
                 "{\"type\":\"sync\",\"data\":{\"width\":%d,\"height\":%d,\"pixels\":%s}}",
                 pixels ? ST7789_WIDTH : LCDVT_COLS,
                 pixels ? (ST7789_HEIGHT - LCD_HEADER_H) : LCDVT_ROWS,
                 pixels ? "true" : "false");
        snprintf(topic_buf, sizeof(topic_buf), "%s/control", s_prefix);
        esp_mqtt_client_publish(s_mqtt, topic_buf, sync_buf, 0, 1, 0);
    }

    if (cJSON_IsString(state)) {
        bool working = strcmp(state->valuestring, "working") == 0;
        state_led_set(working ? LED_STATE_WORKING : LED_STATE_WAITING);
        lcd_vt_set_status(working ? LCD_ST_WORKING : LCD_ST_WAITING,
                          cJSON_IsString(title) ? title->valuestring : NULL);
    }
    cJSON_Delete(root);
}

static void handle_data(esp_mqtt_event_handle_t ev)
{
    char topic[256];
    size_t tlen = ev->topic_len < sizeof(topic) - 1 ? ev->topic_len : sizeof(topic) - 1;
    memcpy(topic, ev->topic, tlen);
    topic[tlen] = 0;

    if (ota_mqtt_on_message(s_mqtt, topic, (const uint8_t *) ev->data, ev->data_len)) {
        return;
    }

    if (ends_with(topic, "/vibetty")) {
        handle_presence(topic, (const char *) ev->data, ev->data_len);
    } else if (ends_with(topic, "/screen_text")) {
        handle_screen_text((const uint8_t *) ev->data, ev->data_len);
    } else if (ends_with(topic, "/screen")) {
        /* JPEG screen frame; may arrive as one or several MQTT_EVENT_DATA
         * events (large payloads are chunked), so hand the fragments to the
         * reassembler with the event offsets. Decode happens later on the app
         * task, not here. */
        screen_jpeg_feed((const uint8_t *) ev->data, ev->data_len,
                         ev->current_data_offset, ev->total_data_len);
    }
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void) arg;
    (void) base;
    esp_mqtt_event_handle_t ev = event_data;

    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_connected = true;
        ESP_LOGI(TAG, "MQTT connected, subscribe %s", DISCOVERY_TOPIC);
        esp_mqtt_client_subscribe(s_mqtt, DISCOVERY_TOPIC, 0);
        ota_mqtt_on_connected(s_mqtt);
        state_led_set(LED_STATE_CONNECTING);
        lcd_vt_set_status(LCD_ST_CONNECTING, NULL);
        break;
    case MQTT_EVENT_ERROR:
        if (ev->error_handle) {
            ESP_LOGE(TAG, "MQTT error type=%d esp_err=%d sock_errno=%d conn_rc=%d",
                     ev->error_handle->error_type,
                     ev->error_handle->esp_tls_last_esp_err,
                     ev->error_handle->esp_transport_sock_errno,
                     ev->error_handle->connect_return_code);
        } else {
            ESP_LOGE(TAG, "MQTT error (no handle)");
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
        s_has_instance = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        state_led_set(LED_STATE_OFFLINE);
        lcd_vt_set_pixels(false);
        screen_jpeg_reset();
        lcd_vt_set_status(LCD_ST_OFFLINE, NULL);
        break;
    case MQTT_EVENT_DATA:
        handle_data(ev);
        break;
    default:
        break;
    }
}

static void mqtt_start(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = nvs_config_broker_uri(),
        /* Inbound buffer sized for /screen JPEG frames (the transcript relay
         * and vibetty -q high publish ~8-16 KB bodies). ESP-MQTT silently drops
         * inbound payloads larger than buffer.size, so this must exceed the
         * biggest screen frame. */
        .buffer.size = 32768,
        .buffer.out_size = 8192,
        /* Persistent session: QoS1 OTA data sent while briefly offline is
         * queued by the broker and delivered on reconnect, letting an OTA
         * survive transient MQTT drops. */
        .session.disable_clean_session = true,
        .session.keepalive = 60,
        /* Generous read timeout: emqx.io from this network sits at ~300 ms RTT
         * with jitter; the old 10 s timeout treated ordinary stalls as dead. */
        .network.timeout_ms = 20000,
    };
    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "wifi disconnected, reconnect");
        state_led_set(LED_STATE_CONNECTING);
        lcd_vt_set_status(LCD_ST_CONNECTING, NULL);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        mqtt_start();
    }
}

esp_err_t wifi_mqtt_init(void)
{
    ota_mqtt_init();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {0};
    strncpy((char *) wifi_cfg.sta.ssid, nvs_config_wifi_ssid(), sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *) wifi_cfg.sta.password, nvs_config_wifi_pass(), sizeof(wifi_cfg.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Keep the radio awake. The default modem-sleep (WIFI_PS_MIN_MODEM) lets
     * the PHY nap between beacons, which on a high-latency overseas broker
     * (emqx.io, ~300 ms RTT) stalls TCP reads just long enough to trip
     * esp-mqtt's "Network timeout while reading" and drop every ~20 s. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    state_led_set(LED_STATE_CONNECTING);
    lcd_vt_set_status(LCD_ST_CONNECTING, NULL);
    ESP_LOGI(TAG, "wifi started, connecting to %s", nvs_config_wifi_ssid());
    return ESP_OK;
}

bool wifi_mqtt_connected(void)
{
    return s_mqtt_connected && s_has_instance;
}

esp_err_t wifi_mqtt_send_key(const uint8_t *bytes, size_t len)
{
    if (!wifi_mqtt_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    char topic[160];
    snprintf(topic, sizeof(topic), "%s/pty_in", s_prefix);
    int ret = esp_mqtt_client_publish(s_mqtt, topic, (const char *) bytes, len, 0, 0);
    return ret == -1 ? ESP_FAIL : ESP_OK;
}

esp_err_t wifi_mqtt_send_text(const char *text)
{
    if (!wifi_mqtt_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    char topic[160];
    snprintf(topic, sizeof(topic), "%s/control", s_prefix);
    char payload[512];
    snprintf(payload, sizeof(payload), "{\"type\":\"input_text\",\"data\":\"%s\"}", text);
    int ret = esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 0);
    return ret == -1 ? ESP_FAIL : ESP_OK;
}
