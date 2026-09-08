#include <string.h>
#include <stdio.h>

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"

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

/* Two known networks the device and PC roam between. AP0 = primary (default
 * phone hotspot; NVS 'ssid' can override), AP1 = fallback, chosen as whichever
 * of these two the primary is NOT so a stored override never duplicates an AP.
 * AP switch triggers on any ESP disconnect reason >= 200 (beacon / no-AP /
 * auth / assoc / handshake failures all mean "this AP is unreachable"). */
#define AP_HOTSPOT_SSID  "Justin\xE7\x9A\x84Mate X6 \xE5\x85\xB8\xE8\x97\x8F\xE7\x89\x88"
#define AP_HOTSPOT_PASS  "18602191868"
#define AP_OFFICE_SSID   "360WiFi-91868"
#define AP_OFFICE_PASS   "18602191868"
#define AP_REPROBE_MS    60000   /* on fallback, re-probe primary every 60 s */

/* mosquitto runs on the PC, so its address depends on which net the STA joined.
 * Both are tried with the current AP's broker first; the disconnect of the
 * other candidate never happens because the device is only ever on one net.
 * An explicit stored NVS 'broker' leads the candidate list if present. */
#define BROKER_OFFICE_URI   "mqtt://192.168.1.15:1883"
#define BROKER_HOTSPOT_URI  "mqtt://192.168.43.125:1883"

/* UDP broker discovery. The PC's mosquitto has a DHCP address that changes on
 * reboot, so before connecting we broadcast a request on the current subnet and
 * let the PC relay reply with the broker address it can actually route to us.
 * Protocol (relay side: build/real/vibecode_relay.py, DISC_PORT): */
#define DISC_PORT       41793
#define DISC_REQ        "VIBEKEY-DISC\n"
#define DISC_RES_PFX    "VIBEKEY-BROKER "
#define DISC_PROBE_MS   20000   /* while up but broker down, re-probe every 20 s */

static const char *TAG = "wifi_mqtt";

static esp_mqtt_client_handle_t s_mqtt = NULL;
static bool s_mqtt_connected = false;
static bool s_has_instance = false;
static char s_prefix[128] = {0};

static int  s_ap_cur = 0;             /* 0 = primary, 1 = fallback */
static bool s_wifi_up = false;        /* STA has an IP (net is usable) */
static char s_ap_ssid[33] = {0};      /* ssid of the AP we last configured */
static char s_own_ip[16] = {0};       /* dotted own IP from GOT_IP */
static char s_disc_ip[40] = {0};      /* broker host learned by UDP discovery */
static char s_disc_uri[128] = {0};    /* mqtt://<s_disc_ip>:1883 */
static esp_timer_handle_t s_ap_timer = NULL;
static esp_timer_handle_t s_probe_timer = NULL;

/* Broker candidates in try order, rebuilt on each fresh association. */
static const char *s_broker_cand[3];
static int         s_broker_n = 0;
static int         s_broker_i = 0;
static int         s_conn_fail = 0;         /* consecutive connect failures */
static bool        s_ever_conn = false;     /* connected once to this client */
static esp_timer_handle_t s_fail_timer = NULL;

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
        s_conn_fail = 0;
        s_ever_conn = true;
        ESP_LOGI(TAG, "MQTT connected to %s, subscribe %s",
                 s_broker_cand[s_broker_i], DISCOVERY_TOPIC);
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
        /* A TCP-transport failure before ever connecting means this broker
         * address is unreachable from the current net (we probed the wrong
         * candidate). Two such failures in a row -> move to the next one. Once
         * a candidate has connected, later drops are broker outages, not wrong
         * candidates, so esp-mqtt's own reconnect to the same URI handles it. */
        if (ev->error_handle &&
            ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT &&
            !s_ever_conn && s_broker_n > 1 && ++s_conn_fail >= 2) {
            int prev = s_broker_i;
            s_conn_fail = 0;
            s_broker_i = (s_broker_i + 1) % s_broker_n;
            ESP_LOGW(TAG, "broker %s unreachable, trying %s",
                     s_broker_cand[prev], s_broker_cand[s_broker_i]);
            esp_timer_start_once(s_fail_timer, 100 * 1000);
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
    if (s_broker_n == 0) {
        s_broker_cand[0] = nvs_config_broker_uri();
        s_broker_n = 1;
        s_broker_i = 0;
    }
    const char *uri = s_broker_cand[s_broker_i];
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
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
        /* Local LAN broker (mosquitto on the PC): a connect is sub-second, so a
         * short read timeout doubles as fast candidate failover instead of
         * stalling on an unreachable IP for many seconds. */
        .network.timeout_ms = 8000,
    };
    ESP_LOGI(TAG, "MQTT client -> %s", uri);
    s_ever_conn = false;
    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt);
}

/* Tear down the MQTT client when the link goes away. Each STA reconnect ends in
 * a fresh IP_EVENT_STA_GOT_IP which calls mqtt_start(); without stopping here a
 * blip would stack a second client on top of the old one. */
static void mqtt_stop(void)
{
    if (s_mqtt != NULL) {
        esp_mqtt_client_destroy(s_mqtt);
        s_mqtt = NULL;
    }
    s_mqtt_connected = false;
    s_has_instance = false;
}

static void broker_cands_add(const char *u)
{
    for (int i = 0; i < s_broker_n; i++) {
        if (strcmp(s_broker_cand[i], u) == 0) {
            return;
        }
    }
    if (s_broker_n < 3) {
        s_broker_cand[s_broker_n++] = u;
    }
}

/* Broker try-list for the current association: the UDP-discovered address (this
 * association, or the NVS cache from last time) leads, then the current net's
 * broker, then the other net's broker. */
static void broker_cands_build(void)
{
    s_broker_n = 0;
    if (s_disc_uri[0]) {
        broker_cands_add(s_disc_uri);
    } else if (nvs_config_broker_stored()) {
        broker_cands_add(nvs_config_broker_uri());
    }
    bool hotspot = strcmp(s_ap_ssid, AP_HOTSPOT_SSID) == 0;
    broker_cands_add(hotspot ? BROKER_HOTSPOT_URI : BROKER_OFFICE_URI);
    broker_cands_add(hotspot ? BROKER_OFFICE_URI : BROKER_HOTSPOT_URI);
    s_broker_i = 0;
    s_conn_fail = 0;
}

/* Move to the next broker candidate. Run from the esp_timer task, never from an
 * MQTT event callback (destroying a client from its own handler is unsafe). */
static void mqtt_failover_do(void *arg)
{
    (void) arg;
    mqtt_stop();
    mqtt_start();
}

/* UDP broker discovery: ask the local subnet "who carries the vibetty broker?"
 * and take the first valid reply. The PC relay answers with the address it can
 * route to us, so this survives any DHCP churn on the PC. Blocking (bounded to
 * ~1 s only when no responder exists; replies normally arrive in ms). */
static bool broker_disc_learn(void)
{
    if (!s_own_ip[0]) {
        return false;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        ESP_LOGW(TAG, "discovery: socket failed");
        return false;
    }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    struct timeval tv = { .tv_sec = 0, .tv_usec = 250000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Send to both the limited and the /24 subnet-directed broadcast so the
     * query survives APs that only forward one of the two forms. */
    int a, b, c, d;
    if (sscanf(s_own_ip, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) {
        a = 0; b = 0; c = 0; d = 0;
    }
    char directed[16];
    snprintf(directed, sizeof(directed), "%d.%d.%d.255", a, b, c);
    const char *targets[2] = { "255.255.255.255", directed };

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(DISC_PORT);
    for (int i = 0; i < 2; i++) {
        dst.sin_addr.s_addr = inet_addr(targets[i]);
        sendto(fd, DISC_REQ, strlen(DISC_REQ), 0, (struct sockaddr *) &dst, sizeof(dst));
    }

    char ip[40] = {0};
    char buf[128];
    bool got = false;
    for (int i = 0; i < 4 && !got; i++) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *) &from, &fromlen);
        if (n <= 0) {
            continue;
        }
        buf[n] = 0;
        if (strncmp(buf, DISC_RES_PFX, strlen(DISC_RES_PFX)) != 0) {
            continue;
        }
        const char *p = buf + strlen(DISC_RES_PFX);
        int j = 0;
        while (j < (int) sizeof(ip) - 1 && p[j] && p[j] != ' ' && p[j] != '\n' && p[j] != '\r') {
            ip[j] = p[j];
            j++;
        }
        ip[j] = 0;
        int x1, x2, x3, x4;
        if (sscanf(ip, "%d.%d.%d.%d", &x1, &x2, &x3, &x4) == 4 &&
            x1 >= 0 && x1 <= 255 && x2 >= 0 && x2 <= 255 &&
            x3 >= 0 && x3 <= 255 && x4 >= 0 && x4 <= 255) {
            got = true;
        }
    }
    close(fd);
    if (!got || !ip[0]) {
        return false;
    }
    strncpy(s_disc_ip, ip, sizeof(s_disc_ip) - 1);
    s_disc_ip[sizeof(s_disc_ip) - 1] = 0;
    snprintf(s_disc_uri, sizeof(s_disc_uri), "mqtt://%s:1883", s_disc_ip);
    ESP_LOGI(TAG, "broker discovery -> %s", s_disc_uri);
    if (strcmp(nvs_config_broker_uri(), s_disc_uri) != 0) {
        nvs_config_save_broker(s_disc_uri);
    }
    return true;
}

/* Periodic re-probe: WiFi is up but the broker is unreachable. The PC's IP may
 * have changed while the device stayed associated (PC reboot), so re-run
 * discovery; only reconnect when the learned host actually differs. */
static void broker_probe_tick(void *arg)
{
    (void) arg;
    if (!s_wifi_up || s_mqtt_connected) {
        return;
    }
    char before[40];
    strncpy(before, s_disc_ip, sizeof(before) - 1);
    before[sizeof(before) - 1] = 0;
    if (!broker_disc_learn()) {
        return;
    }
    if (strcmp(before, s_disc_ip) != 0) {
        ESP_LOGW(TAG, "broker host %s -> %s, reconnecting", before, s_disc_ip);
        mqtt_stop();
        broker_cands_build();
        mqtt_start();
    }
}

static void wifi_ap_set(int ap)
{
    const char *ssid, *pass;
    if (ap == 0) {
        ssid = nvs_config_wifi_ssid();
        pass = nvs_config_wifi_pass();
    } else if (strcmp(nvs_config_wifi_ssid(), AP_HOTSPOT_SSID) == 0) {
        ssid = AP_OFFICE_SSID;
        pass = AP_OFFICE_PASS;
    } else {
        ssid = AP_HOTSPOT_SSID;
        pass = AP_HOTSPOT_PASS;
    }
    wifi_config_t cfg = {0};
    strncpy((char *) cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *) cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "set_config AP%d failed: %s", ap, esp_err_to_name(e));
        return;
    }
    strncpy(s_ap_ssid, ssid, sizeof(s_ap_ssid) - 1);
    ESP_LOGI(TAG, "wifi connecting to AP%d ssid=%s", ap, ssid);
    esp_wifi_connect();
}

/* While parked on the fallback (preferred AP absent), periodically drop the
 * link so the STA reconnect logic returns to the primary once it is back. */
static void ap_pref_tick(void *arg)
{
    (void) arg;
    if (!s_wifi_up || s_ap_cur != 1) {
        return;
    }
    ESP_LOGI(TAG, "on fallback AP, dropping link to re-probe primary");
    s_ap_cur = 0;
    esp_wifi_disconnect();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) data;
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            wifi_ap_set(s_ap_cur);
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *d = data;
            ESP_LOGW(TAG, "wifi disconnected (reason=%d), ap=%d",
                     d ? d->reason : -1, s_ap_cur);
            state_led_set(LED_STATE_CONNECTING);
            lcd_vt_set_status(LCD_ST_CONNECTING, NULL);
            mqtt_stop();
            s_wifi_up = false;
            /* Reasons 0..199 are 802.11 deauths (AP kicked us / transient);
             * reasons >=200 mean the connect attempt itself failed (beacon
             * timeout / no AP / auth / assoc / handshake), i.e. this AP is
             * unreachable -- rotate to the other one. */
            if (d && d->reason >= 200) {
                s_ap_cur = (s_ap_cur + 1) % 2;
                ESP_LOGI(TAG, "AP unreachable, switching to AP%d", s_ap_cur);
            }
            wifi_ap_set(s_ap_cur);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "got IP " IPSTR " (ssid=%s ap=%d)",
                 IP2STR(&ev->ip_info.ip), s_ap_ssid, s_ap_cur);
        s_wifi_up = true;
        snprintf(s_own_ip, sizeof(s_own_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        broker_disc_learn();
        broker_cands_build();
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

    esp_timer_create_args_t targs = { .callback = ap_pref_tick, .name = "ap_pref" };
    if (esp_timer_create(&targs, &s_ap_timer) == ESP_OK) {
        esp_timer_start_periodic(s_ap_timer, AP_REPROBE_MS * 1000);
    }
    esp_timer_create_args_t ftargs = { .callback = mqtt_failover_do, .name = "mqtt_fail" };
    esp_timer_create(&ftargs, &s_fail_timer);
    esp_timer_create_args_t ptargs = { .callback = broker_probe_tick, .name = "brk_probe" };
    esp_timer_create(&ptargs, &s_probe_timer);
    esp_timer_start_periodic(s_probe_timer, DISC_PROBE_MS * 1000);

    state_led_set(LED_STATE_CONNECTING);
    lcd_vt_set_status(LCD_ST_CONNECTING, NULL);
    ESP_LOGI(TAG, "wifi started, primary=%s fallback=%s brokers={%s,%s}",
             nvs_config_wifi_ssid(),
             strcmp(nvs_config_wifi_ssid(), AP_HOTSPOT_SSID) == 0 ? AP_OFFICE_SSID
                                                                  : AP_HOTSPOT_SSID,
             BROKER_OFFICE_URI, BROKER_HOTSPOT_URI);
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
