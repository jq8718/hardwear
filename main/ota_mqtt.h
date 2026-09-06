#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mqtt_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MQTT firmware OTA (Stage 8b). Control topic vkey/<chipid>/ota/ctl and raw
 * data topic vkey/<chipid>/ota/data. Call ota_mqtt_on_connected() when the
 * broker connects (subscribes + restores state) and route every inbound MQTT
 * message through ota_mqtt_on_message() before other handling; it returns true
 * when it consumed the message.
 *
 * Protocol:
 *   ctl  {"cmd":"start","size":N,"md5":"<32hex>"}  -> begin OTA to the
 *        inactive partition. size optional; md5 optional but recommended.
 *   ctl  {"cmd":"abort"}                           -> discard current attempt.
 *   data raw bytes, in order, appended to the image.
 *   status topic vkey/<chipid>/ota/status publishes start/progress/done/error. */

void ota_mqtt_init(void);
void ota_mqtt_on_connected(esp_mqtt_client_handle_t client);
bool ota_mqtt_on_message(esp_mqtt_client_handle_t client, const char *topic,
                         const uint8_t *data, int len);
/* True while an OTA download is in progress; the power monitor must not
 * deep-sleep the device mid-upgrade. */
bool ota_mqtt_busy(void);

#ifdef __cplusplus
}
#endif
