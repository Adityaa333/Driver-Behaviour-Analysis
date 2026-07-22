/* ============================================================================
 * mqtt_client.c
 *
 * Implementation of the MQTT client declared in mqtt_client.h.
 *
 * Architecture: mqtt_publish_task() is the sole owner of the esp-mqtt
 * client handle. All mqtt_client_publish_*() calls from other tasks only
 * ever touch the internal FreeRTOS queue (xQueueSend with a short bounded
 * wait), never the network stack directly. If the queue is full - meaning
 * the device has been disconnected long enough to exhaust buffering
 * capacity - the oldest-style behavior is to drop the new message and log
 * a warning rather than block the calling sensor/scoring task.
 * ========================================================================= */

#include <string.h>
#include <stdio.h>

/* NOTE: ESP-IDF's esp-mqtt component public header is also literally
 * named "mqtt_client.h", identical to this project's own header. The two
 * are disambiguated using standard C preprocessor include semantics:
 *   - A quoted include, #include "mqtt_client.h", searches the directory
 *     of the *current* file first - which is this project's firmware/
 *     directory - so it deterministically resolves to our own header.
 *   - An angle-bracket include, #include <mqtt_client.h>, skips the
 *     current file's directory entirely and searches only the compiler's
 *     -I/-isystem search paths, where ESP-IDF's build system places every
 *     component's public include directory - so it deterministically
 *     resolves to the esp-mqtt library header instead.
 * This is the standard resolution for this exact, well-known ESP-IDF
 * naming collision; it is not compiler- or build-order-dependent. */
#include "mqtt_client.h"        /* This module's own public API (quoted) */
#include "config.h"
#include "wifi_manager.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include <mqtt_client.h>         /* esp-mqtt library (angle-bracket form) */

static const char *TAG = "mqtt_client";

#define DEVICE_ID_HEX_LEN               12   /* 6 MAC bytes -> 12 hex chars */
#define MQTT_CLIENT_ID_MAX_LEN          (sizeof(MQTT_CLIENT_ID_PREFIX) + DEVICE_ID_HEX_LEN)
#define MQTT_CONNECTED_BIT              (1 << 0)
#define MQTT_ENQUEUE_WAIT_MS            200

typedef enum {
    MQTT_MSG_CATEGORY_TELEMETRY = 0,
    MQTT_MSG_CATEGORY_SCORE,
    MQTT_MSG_CATEGORY_ALERT,
    MQTT_MSG_CATEGORY_CRASH,
    MQTT_MSG_CATEGORY_STATUS,
} mqtt_msg_category_t;

typedef struct {
    char topic[MQTT_MAX_TOPIC_LEN];
    char payload[MQTT_MAX_PAYLOAD_LEN];
    int qos;
} mqtt_publish_msg_t;

static QueueHandle_t s_publish_queue = NULL;
static EventGroupHandle_t s_mqtt_event_group = NULL;
static esp_mqtt_client_handle_t s_client = NULL;
static char s_device_id[DEVICE_ID_HEX_LEN + 1] = { 0 };
static bool s_initialized = false;

/* ---------------------------------------------------------------------------
 * Device Identity
 * ------------------------------------------------------------------------- */

static esp_err_t mqtt_generate_device_id(void)
{
    uint8_t mac[6];
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_efuse_mac_get_default failed: %s", esp_err_to_name(err));
        return err;
    }

    snprintf(s_device_id, sizeof(s_device_id), "%02X%02X%02X%02X%02X%02X",
              mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * MQTT Event Handling
 * ------------------------------------------------------------------------- */

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected to broker");
            xEventGroupSetBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected from broker");
            xEventGroupClearBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
            break;

        case MQTT_EVENT_ERROR:
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(TAG, "MQTT transport error: esp_tls errno=%d, tls_stack_err=%d",
                         event->error_handle->esp_transport_sock_errno,
                         event->error_handle->esp_tls_stack_err);
            } else {
                ESP_LOGE(TAG, "MQTT error, type=%d", event->error_handle->error_type);
            }
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG, "MQTT publish acknowledged, msg_id=%d", event->msg_id);
            break;

        default:
            break;
    }
}

/* ---------------------------------------------------------------------------
 * Background Publish Task
 * ------------------------------------------------------------------------- */

static void mqtt_publish_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Waiting for WiFi connection before starting MQTT client");
    while (wifi_manager_wait_connected(portMAX_DELAY) != ESP_OK) {
        /* portMAX_DELAY blocks indefinitely; this loop only exists as a
         * defensive guard in case of an unexpected early return. */
    }

    char client_id[MQTT_CLIENT_ID_MAX_LEN];
    snprintf(client_id, sizeof(client_id), "%s%s", MQTT_CLIENT_ID_PREFIX, s_device_id);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.client_id = client_id,
        .session.keepalive = MQTT_KEEPALIVE_SEC,
        .network.reconnect_timeout_ms = MQTT_RECONNECT_TIMEOUT_MS,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed, MQTT task exiting");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                     mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_register_event failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "MQTT client started, client_id=%s", client_id);

    mqtt_publish_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_publish_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* esp-mqtt internally buffers (outboxes) QoS>0 messages while
         * disconnected and flushes them on reconnect, so we publish
         * unconditionally here rather than checking connection state
         * first; checking here would only create a race against the
         * event handler updating the connected bit. */
        int msg_id = esp_mqtt_client_publish(s_client, msg.topic, msg.payload,
                                              (int)strlen(msg.payload), msg.qos, 0);
        if (msg_id < 0) {
            ESP_LOGW(TAG, "esp_mqtt_client_publish failed for topic %s", msg.topic);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Publish Queue Helper
 * ------------------------------------------------------------------------- */

static esp_err_t mqtt_enqueue(const char *topic_fmt, int qos, const char *json_payload)
{
    if (json_payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(json_payload) >= MQTT_MAX_PAYLOAD_LEN) {
        ESP_LOGE(TAG, "Payload too long (%d bytes), discarding", (int)strlen(json_payload));
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    mqtt_publish_msg_t msg;
    snprintf(msg.topic, sizeof(msg.topic), topic_fmt, s_device_id);
    strncpy(msg.payload, json_payload, sizeof(msg.payload) - 1);
    msg.payload[sizeof(msg.payload) - 1] = '\0';
    msg.qos = qos;

    if (xQueueSend(s_publish_queue, &msg, pdMS_TO_TICKS(MQTT_ENQUEUE_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Publish queue full, dropping message for topic %s", msg.topic);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t mqtt_client_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = mqtt_generate_device_id();
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    s_mqtt_event_group = xEventGroupCreate();
    if (s_mqtt_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create MQTT event group");
        return ESP_ERR_NO_MEM;
    }

    s_publish_queue = xQueueCreate(QUEUE_LEN_MQTT_PUBLISH, sizeof(mqtt_publish_msg_t));
    if (s_publish_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create MQTT publish queue");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_created = xTaskCreatePinnedToCore(
        mqtt_publish_task, "mqtt_publish_task", TASK_STACK_SIZE_MQTT_PUBLISH, NULL,
        TASK_PRIORITY_MQTT_PUBLISH, NULL, TASK_CORE_NETWORKING);

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create MQTT publish task");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "MQTT client module initialized, device_id=%s", s_device_id);
    return ESP_OK;
}

bool mqtt_client_is_connected(void)
{
    if (s_mqtt_event_group == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_mqtt_event_group) & MQTT_CONNECTED_BIT) != 0;
}

const char *mqtt_client_get_device_id(void)
{
    return s_device_id;
}

esp_err_t mqtt_client_publish_telemetry(const char *json_payload)
{
    return mqtt_enqueue(MQTT_TOPIC_TELEMETRY_FMT, MQTT_QOS_TELEMETRY, json_payload);
}

esp_err_t mqtt_client_publish_score(const char *json_payload)
{
    return mqtt_enqueue(MQTT_TOPIC_SCORE_FMT, MQTT_QOS_TELEMETRY, json_payload);
}

esp_err_t mqtt_client_publish_alert(const char *json_payload)
{
    return mqtt_enqueue(MQTT_TOPIC_ALERT_FMT, MQTT_QOS_ALERT, json_payload);
}

esp_err_t mqtt_client_publish_crash(const char *json_payload)
{
    return mqtt_enqueue(MQTT_TOPIC_CRASH_FMT, MQTT_QOS_CRASH, json_payload);
}

esp_err_t mqtt_client_publish_status(const char *json_payload)
{
    return mqtt_enqueue(MQTT_TOPIC_STATUS_FMT, MQTT_QOS_STATUS, json_payload);
}
