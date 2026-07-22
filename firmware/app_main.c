/* ============================================================================
 * app_main.c
 *
 * Application-level bootstrap for the Driver Behaviour Analysis System.
 * Separated from main.c so that the ESP-IDF-required entry point
 * (app_main() in main.c) stays a trivial one-line handoff, while all
 * actual startup logic - log configuration, the startup banner, default
 * geofence provisioning, and delegating to task_manager - lives here.
 * ========================================================================= */

#include <stdio.h>
#include "config.h"
#include "task_manager.h"
#include "geofence.h"
#include "esp_log.h"

static const char *TAG = "app_main";

/**
 * @brief Register the default geofence zone(s) for this deployment.
 *
 * ASSUMPTION: these coordinates are illustrative (San Francisco, CA) and
 * must be replaced with the actual depot/service-area coordinates for a
 * real deployment. In the absence of a remote provisioning mechanism
 * (see the note in geofence.h about MQTT subscribe support not yet
 * existing), this is the current mechanism for configuring zones: edit
 * and redeploy firmware, or extend this function to read zone
 * definitions from NVS at boot.
 */
static void register_default_geofence_zones(void)
{
    uint8_t zone_index = 0;
    esp_err_t err = geofence_add_zone("Depot", 37.7749, -122.4194, 500.0f,
                                       GEOFENCE_ZONE_ALLOWED, &zone_index);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register default depot geofence zone: %s",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Default depot geofence zone registered at index %d", zone_index);
    }
}

/**
 * @brief Real application entry point, called from app_main() in main.c.
 */
void app_main_run(void)
{
    /* INFO is a reasonable default verbosity for a deployed fleet
     * device; DEBUG-level driver logging in mpu6050/gps/obd would be
     * far too noisy for continuous operation but is available by
     * raising this during bring-up/troubleshooting. */
    esp_log_level_set("*", ESP_LOG_INFO);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " %s", DEVICE_TYPE_STRING);
    ESP_LOGI(TAG, " Driver Behaviour Analysis System");
    ESP_LOGI(TAG, " Firmware v%d.%d.%d",
             FIRMWARE_VERSION_MAJOR, FIRMWARE_VERSION_MINOR, FIRMWARE_VERSION_PATCH);
    ESP_LOGI(TAG, "========================================");

    task_manager_start_all();

    /* geofence_init() runs as part of task_manager_start_all(); zones
     * can only be registered after that has completed. */
    register_default_geofence_zones();

    ESP_LOGI(TAG, "Startup sequence complete; all modules running as background tasks");
}
