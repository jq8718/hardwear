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

/* WiFi / broker credentials and target instance now come from NVS
 * (nvs_config), so a reflash no longer hard-codes the network. */
#define DISCOVERY_TOPIC  "+/+/+/vibetty"

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
    ESP_LOGI(TAG, "screen_text tag=%d bytes=%d", tag, text_len);
    if (tag == 0x00) {
        // Full-frame baseline: log a short preview (raw ANSI, no full render yet).
        ESP_LOGI(TAG, "screen full: %.80s", (const char *)(data + 1));
    }
}

static void handle_presence(const char *topic, const char *data, int len)
{
    (void) topic;
    if (len == 0) {
        s_has_instance = false;
        ESP_LOGW(TAG, "instance offline (LWT)");
        state_led_set(LED_STATE_OFFLINE);
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
        if (target[0] && strcmp(target, prefix->valuestring) != 0) {
            ESP_LOGI(TAG, "presence prefix=%s not our target %s, ignore",
                     prefix->valuestring, target);
            cJSON_Delete(root);
            return;
        }

        strncpy(s_prefix, prefix->valuestring, sizeof(s_prefix) - 1);
        s_has_instance = true;
        if (!target[0]) {
            /* First adoption: lock this instance in NVS so later random
             * presences cannot hijack s_prefix (multi-instance safety). */
            nvs_config_save_target_prefix(s_prefix);
        }

        bool text_mode = strcmp(format->valuestring, "text") == 0;
        char topic_buf[160];
        snprintf(topic_buf, sizeof(topic_buf), "%s/%s", s_prefix,
                 text_mode ? "screen_text" : "screen");
        esp_mqtt_client_subscribe(s_mqtt, topic_buf, 0);

        ESP_LOGI(TAG, "instance prefix=%s format=%s title=%s",
                 s_prefix, format->valuestring,
                 cJSON_IsString(title) ? title->valuestring : "?");

        char sync_buf[128];
        snprintf(sync_buf, sizeof(sync_buf),
                 "{\"type\":\"sync\",\"data\":{\"width\":80,\"height\":24,\"pixels\":false}}");
        snprintf(topic_buf, sizeof(topic_buf), "%s/control", s_prefix);
        esp_mqtt_client_publish(s_mqtt, topic_buf, sync_buf, 0, 1, 0);
    }

    if (cJSON_IsString(state)) {
        state_led_set(strcmp(state->valuestring, "working") == 0
                      ? LED_STATE_WORKING : LED_STATE_WAITING);
    }
    cJSON_Delete(root);
}

static void handle_data(esp_mqtt_event_handle_t ev)
{
    char topic[256];
    size_t tlen = ev->topic_len < sizeof(topic) - 1 ? ev->topic_len : sizeof(topic) - 1;
    memcpy(topic, ev->topic, tlen);
    topic[tlen] = 0;

    if (ends_with(topic, "/vibetty")) {
        handle_presence(topic, (const char *) ev->data, ev->data_len);
    } else if (ends_with(topic, "/screen_text")) {
        handle_screen_text((const uint8_t *) ev->data, ev->data_len);
    } else if (ends_with(topic, "/screen")) {
        ESP_LOGI(TAG, "screen JPEG %d bytes (decode deferred)", ev->data_len);
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
        state_led_set(LED_STATE_CONNECTING);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
        s_has_instance = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        state_led_set(LED_STATE_OFFLINE);
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
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        mqtt_start();
    }
}

esp_err_t wifi_mqtt_init(void)
{
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

    state_led_set(LED_STATE_CONNECTING);
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
