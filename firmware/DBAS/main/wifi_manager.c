/* ============================================================================
 * wifi_manager.c
 *
 * Implementation of the WiFi manager declared in wifi_manager.h.
 *
 * Connection state is tracked with a FreeRTOS event group so that other
 * tasks (mqtt_client's connection task in particular) can block on
 * wifi_manager_wait_connected() rather than polling. Reconnection is
 * entirely event-driven: WIFI_EVENT_STA_DISCONNECTED triggers either an
 * immediate retry (while under WIFI_MAX_RETRY_COUNT) or a one-shot esp_timer
 * backoff of WIFI_RECONNECT_DELAY_MS, repeating indefinitely.
 * ========================================================================= */

#include <string.h>
#include "wifi_manager.h"
#include "config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <inttypes.h>

static const char *TAG = "wifi_manager";

/* Event group bit set once the station has obtained an IP address. */
#define WIFI_CONNECTED_BIT   BIT0

static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_timer_handle_t s_reconnect_timer = NULL;
static uint32_t s_retry_count = 0;
static bool s_initialized = false;

/* ---------------------------------------------------------------------------
 * Reconnection Backoff Timer
 * ------------------------------------------------------------------------- */

static void wifi_reconnect_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Backoff elapsed, attempting to reconnect");
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect (post-backoff) failed: %s", esp_err_to_name(err));
    }
}

/* ---------------------------------------------------------------------------
 * Event Handlers
 * ------------------------------------------------------------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Initial esp_wifi_connect failed: %s", esp_err_to_name(err));
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "WiFi disconnected (reason %d)", disconn->reason);

        if (s_retry_count < WIFI_MAX_RETRY_COUNT) {
            s_retry_count++;
            ESP_LOGI(TAG, "Retrying connection (%" PRIu32 "/%" PRIu32 ")", s_retry_count, (uint32_t)WIFI_MAX_RETRY_COUNT);
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "esp_wifi_connect (retry) failed: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGW(TAG, "Max immediate retries reached, backing off for %d ms",
                     WIFI_RECONNECT_DELAY_MS);
            s_retry_count = 0;
            esp_err_t err = esp_timer_start_once(
                s_reconnect_timer, (uint64_t)WIFI_RECONNECT_DELAY_MS * 1000ULL);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to arm reconnect timer: %s", esp_err_to_name(err));
            }
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ip_event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected, IP: " IPSTR, IP2STR(&ip_event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        return;
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t wifi_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* The WiFi driver requires NVS for calibration data storage. Handle
     * the "needs erase" cases the same way the ESP-IDF examples do. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase, reformatting");
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_erase failed: %s", esp_err_to_name(err));
            return ESP_ERR_INVALID_STATE;
        }
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create WiFi event group");
        return ESP_ERR_NO_MEM;
    }

    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        /* ESP_ERR_INVALID_STATE means the default loop already exists,
         * which is fine if another module created it first. */
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WIFI_EVENT handler: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP_EVENT handler: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = &wifi_reconnect_timer_cb,
        .arg = NULL,
        .name = "wifi_reconnect",
    };
    err = esp_timer_create(&timer_args, &s_reconnect_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
        return ESP_ERR_NO_MEM;
    }

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi manager initialized, connecting to \"%s\"", WIFI_SSID);
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    if (s_wifi_event_group == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t wifi_manager_wait_connected(uint32_t timeout_ms)
{
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                                            pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));

    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_manager_get_rssi(int8_t *rssi_dbm)
{
    if (rssi_dbm == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!wifi_manager_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_sta_get_ap_info failed: %s", esp_err_to_name(err));
        return err;
    }

    *rssi_dbm = ap_info.rssi;
    return ESP_OK;
}
