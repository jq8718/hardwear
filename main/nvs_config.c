#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "nvs_config.h"

#define NVS_NS   "vkey"
#define TAG      "nvs_config"

#define DEFAULT_SSID   "360WiFi-91868"
#define DEFAULT_PASS   "18602191868"
#define DEFAULT_BROKER "mqtt://broker.emqx.io:1883"

static char s_ssid[64];
static char s_pass[64];
static char s_broker[128];
static char s_prefix[128];
static bool s_loaded = false;

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t cap,
                     const char *def)
{
    size_t len = cap;
    esp_err_t err = nvs_get_str(h, key, dst, &len);
    if (err != ESP_OK) {
        strncpy(dst, def, cap - 1);
        dst[cap - 1] = 0;
        ESP_LOGW(TAG, "key %s not found (%s), using default", key, esp_err_to_name(err));
    }
}

esp_err_t nvs_config_init(void)
{
    if (s_loaded) {
        return ESP_OK;
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    nvs_handle_t h;
    err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    load_str(h, "ssid", s_ssid, sizeof(s_ssid), DEFAULT_SSID);
    load_str(h, "pass", s_pass, sizeof(s_pass), DEFAULT_PASS);
    load_str(h, "broker", s_broker, sizeof(s_broker), DEFAULT_BROKER);
    load_str(h, "vprefix", s_prefix, sizeof(s_prefix), "");
    nvs_close(h);
    s_loaded = true;

    ESP_LOGI(TAG, "loaded: ssid=%s broker=%s target_prefix=%s%s",
             s_ssid, s_broker, s_prefix[0] ? s_prefix : "(none)",
             s_prefix[0] ? "" : " (will persist first discovered)");
    return ESP_OK;
}

const char *nvs_config_wifi_ssid(void)       { return s_ssid; }
const char *nvs_config_wifi_pass(void)       { return s_pass; }
const char *nvs_config_broker_uri(void)      { return s_broker; }
const char *nvs_config_target_prefix(void)   { return s_prefix; }

esp_err_t nvs_config_save_target_prefix(const char *prefix)
{
    if (!prefix) {
        prefix = "";
    }
    strncpy(s_prefix, prefix, sizeof(s_prefix) - 1);
    s_prefix[sizeof(s_prefix) - 1] = 0;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    if (s_prefix[0]) {
        err = nvs_set_str(h, "vprefix", s_prefix);
    } else {
        err = nvs_erase_key(h, "vprefix");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "target prefix %s%s", s_prefix[0] ? s_prefix : "(cleared)",
             err == ESP_OK ? " saved" : " FAILED");
    return err;
}
