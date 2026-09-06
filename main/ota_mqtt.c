#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_rom_md5.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "ota_mqtt.h"

#define OTA_TAG       "ota_mqtt"
#define CHUNK_MAX     7900        /* payload bytes per frame */
#define FRAME_HDR     6           /* uint32 offset LE + uint16 payload len LE */
#define MAX_FRAME     (CHUNK_MAX + FRAME_HDR)
#define ABORT_TIMEOUT_US (200 * 1000 * 1000)
#define SB_SIZE       (512 * 1024)
#define OTA_TASK_STACK 8192
#define NEED_MIN_GAP_MS 300

typedef enum {
    OTA_IDLE = 0,
    OTA_RECEIVING,
} ota_state_t;

static const char *TAG = OTA_TAG;

static char s_base[64];
static esp_mqtt_client_handle_t s_client = NULL;
static volatile ota_state_t s_state = OTA_IDLE;

/* Slow flash writes run in the worker task, fed by a PSRAM stream buffer, so
 * sustained chunk flow never starves the MQTT/network path. Data frames carry
 * their absolute flash offset; the worker writes strictly sequentially and
 * discards stale/duplicate/gap frames, so transient broker drops and QoS1
 * redeliveries self-heal: the device republishes its current write position
 * and the host resumes exactly there. */
static esp_ota_handle_t s_ota;
static const esp_partition_t *s_part;
static size_t s_total;
static size_t s_flashed;
static uint8_t s_expect_md5[16];
static bool s_have_md5;
static md5_context_t s_md5_ctx;
static esp_timer_handle_t s_abort_timer;

static StreamBufferHandle_t s_sb;
static StaticStreamBuffer_t s_sb_ctrl;
static uint8_t *s_sb_storage;
static TaskHandle_t s_ota_task;

static volatile bool s_abort_pending;
static char s_abort_why[16];
static volatile TickType_t s_last_need;
/* Single worker task: one shared receive buffer avoids an 8 KB stack frame. */
static uint8_t s_pbuf[CHUNK_MAX];

/* HTTP OTA (alternative transport): the worker fetches the image over plain
 * HTTP from a URL supplied in the start ctl, bypassing the MQTT chunk stream
 * when the broker link is too flaky for a multi-MB transfer. */
static char s_http_url[320];
static volatile bool s_http_pending;

static void ota_worker(void *arg);
static void abort_req(const char *why);

static const char *ota_part_name(const esp_partition_t *p)
{
    static char name[8];
    int slot = p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
    if (p->type == ESP_PARTITION_TYPE_APP && slot >= 0 && slot <= 15) {
        snprintf(name, sizeof(name), "ota_%d", slot);
    } else {
        snprintf(name, sizeof(name), "app");
    }
    return name;
}

static void publish_status(const char *json)
{
    if (!s_client) {
        return;
    }
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/ota/status", s_base);
    /* Status (rdy/start/done/error) is QoS0: it carries the write offset and is
     * re-sent by the worker while it idles, so a lost copy self-heals without
     * broker-side inflight state. */
    esp_mqtt_client_publish(s_client, topic, json, 0, 0, 0);
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex(const char *hex, size_t n, uint8_t *out)
{
    if (!hex || strlen(hex) != n * 2) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t) ((hi << 4) | lo);
    }
    return true;
}

static void stop_abort_timer(void)
{
    if (s_abort_timer) {
        esp_timer_stop(s_abort_timer);
        esp_timer_delete(s_abort_timer);
        s_abort_timer = NULL;
    }
}

static void kick_abort_timer(void)
{
    if (s_abort_timer) {
        esp_timer_stop(s_abort_timer);
        esp_timer_start_once(s_abort_timer, ABORT_TIMEOUT_US);
    }
}

static void abort_timer_cb(void *arg)
{
    (void) arg;
    abort_req("timeout");
}

static void reset_to_idle(void)
{
    s_state = OTA_IDLE;
    s_abort_pending = false;
    if (s_sb) {
        xStreamBufferReset(s_sb);
    }
}

static void worker_abort(void)
{
    const char *why = s_abort_why[0] ? s_abort_why : "abort";
    ESP_LOGW(TAG, "OTA abort: %s", why);
    if (s_state == OTA_RECEIVING) {
        esp_ota_abort(s_ota);
    }
    stop_abort_timer();
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ev\":\"error\",\"code\":\"abort\",\"why\":\"%s\"}", why);
    publish_status(buf);
    reset_to_idle();
}

static void worker_finish(void)
{
    if (s_have_md5) {
        uint8_t digest[16];
        esp_rom_md5_final(digest, &s_md5_ctx);
        if (memcmp(digest, s_expect_md5, 16) != 0) {
            ESP_LOGE(TAG, "md5 mismatch, aborting");
            strncpy(s_abort_why, "md5", sizeof(s_abort_why) - 1);
            s_abort_why[sizeof(s_abort_why) - 1] = 0;
            s_abort_pending = true;
            worker_abort();
            return;
        }
    }

    esp_err_t err = esp_ota_end(s_ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        stop_abort_timer();
        publish_status("{\"ev\":\"error\",\"code\":\"end\"}");
        reset_to_idle();
        return;
    }

    err = esp_ota_set_boot_partition(s_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(err));
        stop_abort_timer();
        publish_status("{\"ev\":\"error\",\"code\":\"setboot\"}");
        reset_to_idle();
        return;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"ev\":\"done\",\"boot\":\"%s\",\"ver\":\"%s\"}",
             ota_part_name(s_part),
             app ? app->version : "?");
    publish_status(buf);
    ESP_LOGI(TAG, "OTA complete on %s, rebooting", buf);

    stop_abort_timer();
    reset_to_idle();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* Tell the host our current write position so it resumes exactly from here. */
static void publish_need(void)
{
    char buf[96];
    snprintf(buf, sizeof(buf),
             "{\"ev\":\"rdy\",\"written\":%u,\"of\":%u,\"part\":\"%s\"}",
             (unsigned) s_flashed, (unsigned) s_total,
             ota_part_name(s_part));
    publish_status(buf);
    s_last_need = xTaskGetTickCount();
}

static size_t read_n(uint8_t *buf, size_t want, int wait_ms)
{
    size_t got = 0;
    while (got < want) {
        size_t n = xStreamBufferReceive(s_sb, buf + got, want - got,
                                        pdMS_TO_TICKS(wait_ms));
        got += n;
        if (n == 0) {
            break;
        }
    }
    return got;
}

static esp_err_t http_ota_event(esp_http_client_event_handle_t ev)
{
    if (ev->event_id == HTTP_EVENT_ON_DATA && ev->data && ev->data_len > 0) {
        if (s_state != OTA_RECEIVING) {
            return ESP_OK;
        }
        esp_rom_md5_update(&s_md5_ctx, ev->data, ev->data_len);
        esp_err_t err = esp_ota_write(s_ota, ev->data, ev->data_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "http ota_write: %s", esp_err_to_name(err));
            return err;
        }
        s_flashed += ev->data_len;
        if (s_total && s_flashed > s_total) {
            ESP_LOGE(TAG, "http image larger than expected");
            return ESP_ERR_INVALID_SIZE;
        }
        if ((s_flashed % (256 * 1024)) < ev->data_len) {
            ESP_LOGI(TAG, "http flashed %u/%u", (unsigned) s_flashed,
                     (unsigned) s_total);
        }
    }
    return ESP_OK;
}

static void http_download_and_flash(void)
{
    esp_http_client_config_t cfg = {
        .url = s_http_url,
        .event_handler = http_ota_event,
        .buffer_size = 4096,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        strncpy(s_abort_why, "http init", sizeof(s_abort_why) - 1);
        s_abort_why[sizeof(s_abort_why) - 1] = 0;
        s_abort_pending = true;
        return;
    }
    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http download: %s", esp_err_to_name(err));
        strncpy(s_abort_why, "http", sizeof(s_abort_why) - 1);
        s_abort_why[sizeof(s_abort_why) - 1] = 0;
        s_abort_pending = true;
        return;
    }
    if (s_total && s_flashed != s_total) {
        ESP_LOGE(TAG, "http got %u bytes, expected %u", (unsigned) s_flashed,
                 (unsigned) s_total);
        strncpy(s_abort_why, "size", sizeof(s_abort_why) - 1);
        s_abort_why[sizeof(s_abort_why) - 1] = 0;
        s_abort_pending = true;
        return;
    }
    /* worker_finish() runs the md5 check, ota_end, set_boot and reboot. */
    if (s_total && s_flashed >= s_total) {
        worker_finish();
    } else {
        abort_req("empty");
    }
}

static void ota_worker(void *arg)
{
    (void) arg;
    size_t last_log = 0;

    for (;;) {
        if (s_abort_pending) {
            worker_abort();
            continue;
        }
        if (s_state != OTA_RECEIVING) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (s_http_pending) {
            s_http_pending = false;
            http_download_and_flash();
            continue;
        }

        uint8_t hdr[FRAME_HDR];
        if (read_n(hdr, FRAME_HDR, 250) != FRAME_HDR) {
            /* Stream empty: ask host for the next block. */
            if (xTaskGetTickCount() - s_last_need > pdMS_TO_TICKS(NEED_MIN_GAP_MS)) {
                publish_need();
            }
            continue;
        }

        uint32_t off = (uint32_t) hdr[0] | ((uint32_t) hdr[1] << 8) |
                       ((uint32_t) hdr[2] << 16) | ((uint32_t) hdr[3] << 24);
        uint16_t plen = (uint16_t) hdr[4] | ((uint16_t) hdr[5] << 8);
        if (plen == 0 || plen > CHUNK_MAX) {
            abort_req("badframe");
            continue;
        }
        if (read_n(s_pbuf, plen, 500) != plen) {
            abort_req("trunc");
            continue;
        }

        if (off < s_flashed) {
            continue;              /* stale/duplicate frame: skip */
        }
        if (off > s_flashed) {
            continue;              /* gap (lost frames): skip; need will resync */
        }

        esp_rom_md5_update(&s_md5_ctx, s_pbuf, plen);
        esp_err_t err = esp_ota_write(s_ota, s_pbuf, plen);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            strncpy(s_abort_why, "write", sizeof(s_abort_why) - 1);
            s_abort_why[sizeof(s_abort_why) - 1] = 0;
            s_abort_pending = true;
            continue;
        }
        s_flashed += plen;

        if (s_flashed - last_log >= 256 * 1024) {
            last_log = s_flashed;
            ESP_LOGI(TAG, "flashed %u/%u bytes", (unsigned) s_flashed,
                     (unsigned) s_total);
        }

        if (s_total && s_flashed >= s_total) {
            worker_finish();
        } else if (xStreamBufferBytesAvailable(s_sb) == 0) {
            publish_need();         /* buffered block drained: fetch next */
        }
    }
}

static void abort_req(const char *why)
{
    if (s_state != OTA_RECEIVING) {
        return;
    }
    strncpy(s_abort_why, why, sizeof(s_abort_why) - 1);
    s_abort_why[sizeof(s_abort_why) - 1] = 0;
    s_abort_pending = true;
}

static void ota_start(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return;
    }
    size_t size = 0;
    const char *md5 = NULL;
    const char *url = NULL;
    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "abort") == 0) {
        cJSON_Delete(root);
        abort_req("user abort");
        return;
    }
    cJSON *sizej = cJSON_GetObjectItem(root, "size");
    if (cJSON_IsNumber(sizej)) {
        size = (size_t) sizej->valuedouble;
    }
    cJSON *md5j = cJSON_GetObjectItem(root, "md5");
    if (cJSON_IsString(md5j)) {
        md5 = md5j->valuestring;
    }
    cJSON *urlj = cJSON_GetObjectItem(root, "url");
    if (cJSON_IsString(urlj)) {
        url = urlj->valuestring;
    }

    if (s_state != OTA_IDLE || s_abort_pending) {
        ESP_LOGW(TAG, "ota busy, ignore new start");
        publish_status("{\"ev\":\"error\",\"code\":\"busy\"}");
        cJSON_Delete(root);
        return;
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "no OTA partition to write");
        publish_status("{\"ev\":\"error\",\"code\":\"no_partition\"}");
        cJSON_Delete(root);
        return;
    }
    if (size > 0 && size > part->size) {
        ESP_LOGE(TAG, "image %u too big for %s (%u)", (unsigned) size,
                 ota_part_name(part), (unsigned) part->size);
        publish_status("{\"ev\":\"error\",\"code\":\"too_big\"}");
        cJSON_Delete(root);
        return;
    }

    esp_err_t err = esp_ota_begin(part, size ? size : OTA_WITH_SEQUENTIAL_WRITES, &s_ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        publish_status("{\"ev\":\"error\",\"code\":\"begin\"}");
        cJSON_Delete(root);
        return;
    }

    if (s_sb) {
        xStreamBufferReset(s_sb);
    }
    s_part = part;
    s_total = size;
    s_flashed = 0;
    s_have_md5 = md5 && parse_hex(md5, 16, s_expect_md5);
    esp_rom_md5_init(&s_md5_ctx);
    s_abort_pending = false;
    s_state = OTA_RECEIVING;
    s_last_need = xTaskGetTickCount();

    bool http = false;
    if (url) {
        if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
            ESP_LOGW(TAG, "unsupported url scheme");
            publish_status("{\"ev\":\"error\",\"code\":\"url\"}");
            reset_to_idle();
            cJSON_Delete(root);
            return;
        }
        snprintf(s_http_url, sizeof(s_http_url), "%s", url);
        s_http_pending = true;
        http = true;
    }

    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"ev\":\"start\",\"size\":%u,\"written\":0,\"part\":\"%s\"}",
             (unsigned) s_total, ota_part_name(s_part));
    publish_status(buf);
    ESP_LOGI(TAG, "OTA begin: %u bytes -> %s via %s%s", (unsigned) s_total,
             ota_part_name(s_part), http ? "http" : "mqtt",
             s_have_md5 ? " (md5 checked)" : "");
    if (http) {
        ESP_LOGI(TAG, "fetch %s", s_http_url);
    }

    if (!http) {
        const esp_timer_create_args_t targs = {
            .callback = abort_timer_cb,
            .arg = NULL,
            .name = "ota_abort",
        };
        if (esp_timer_create(&targs, &s_abort_timer) != ESP_OK) {
            ESP_LOGW(TAG, "abort timer create failed");
        } else {
            kick_abort_timer();
        }
    }
    cJSON_Delete(root);
}

static void ota_handle_data(const uint8_t *data, int len)
{
    if (s_state != OTA_RECEIVING) {
        return;
    }
    if (len < FRAME_HDR || len > MAX_FRAME) {
        ESP_LOGW(TAG, "bad frame len %d", len);
        return;
    }
    uint16_t plen = (uint16_t) data[4] | ((uint16_t) data[5] << 8);
    if (plen == 0 || plen > CHUNK_MAX || len != FRAME_HDR + plen) {
        ESP_LOGW(TAG, "bad frame header");
        return;
    }

    size_t w = xStreamBufferSend(s_sb, data, len, pdMS_TO_TICKS(30));
    if (w != (size_t) len) {
        ESP_LOGE(TAG, "stream buffer overflow");
        abort_req("overflow");
        return;
    }
    kick_abort_timer();
}

void ota_mqtt_init(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_base, sizeof(s_base),
             "vkey/%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    s_state = OTA_IDLE;
    s_abort_timer = NULL;
    s_abort_pending = false;

    s_sb_storage = heap_caps_malloc(SB_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_sb_storage) {
        ESP_LOGW(TAG, "no PSRAM for ota buffer, using internal");
        s_sb_storage = malloc(SB_SIZE);
    }
    if (!s_sb_storage) {
        ESP_LOGE(TAG, "ota stream buffer alloc failed");
        s_sb = NULL;
    } else {
        s_sb = xStreamBufferCreateStatic(SB_SIZE, 1, s_sb_storage, &s_sb_ctrl);
        if (xTaskCreate(ota_worker, "ota", OTA_TASK_STACK, NULL, 5, &s_ota_task) != pdPASS) {
            ESP_LOGE(TAG, "ota worker task create failed");
        }
    }
    ESP_LOGI(TAG, "ota base topic %s (sb=%s)", s_base,
             s_sb ? "ok" : "unavailable");
}

void ota_mqtt_on_connected(esp_mqtt_client_handle_t client)
{
    s_client = client;
    char t[128];
    snprintf(t, sizeof(t), "%s/ota/ctl", s_base);
    esp_mqtt_client_subscribe(client, t, 1);
    snprintf(t, sizeof(t), "%s/ota/data", s_base);
    /* Data is QoS0: frames carry their absolute offset, so the worker can
     * resume from a gap without relying on broker QoS1 redelivery (which
     * also proved to destabilize sustained streams on the public broker). */
    esp_mqtt_client_subscribe(client, t, 0);
    ESP_LOGI(TAG, "subscribed %s/ota/{ctl,data}", s_base);
}

bool ota_mqtt_on_message(esp_mqtt_client_handle_t client, const char *topic,
                         const uint8_t *data, int len)
{
    if (!topic || !s_base[0]) {
        return false;
    }
    size_t bl = strlen(s_base);
    if (strncmp(topic, s_base, bl) != 0) {
        return false;
    }
    const char *suffix = topic + bl;

    if (strcmp(suffix, "/ota/ctl") == 0) {
        char *copy = malloc(len + 1);
        if (copy) {
            memcpy(copy, data, len);
            copy[len] = 0;
            ota_start(copy);
            free(copy);
        }
        return true;
    }

    if (strcmp(suffix, "/ota/data") == 0) {
        ota_handle_data(data, len);
        return true;
    }
    return false;
}
