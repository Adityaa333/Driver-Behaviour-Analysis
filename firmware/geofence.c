/* ============================================================================
 * geofence.c
 *
 * Implementation of the geofence module declared in geofence.h.
 *
 * Distance from the vehicle's current position to each zone center is
 * computed with the Haversine great-circle formula, which is accurate
 * enough for zone radii from tens of meters to tens of kilometers -
 * comfortably covering realistic fleet geofencing use cases - without
 * the complexity of an ellipsoidal (Vincenty) calculation.
 * ========================================================================= */

#include <string.h>
#include <math.h>
#include "geofence.h"
#include "config.h"
#include "sensor_manager.h"
#include "driver_score.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "geofence";

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define EARTH_RADIUS_M           6371000.0

typedef struct {
    char name[GEOFENCE_NAME_MAX_LEN];
    double center_lat_deg;
    double center_lon_deg;
    float radius_m;
    geofence_zone_type_t type;
    bool in_use;
    bool state_known;
    bool currently_inside;
} geofence_zone_state_t;

static SemaphoreHandle_t s_zones_mutex = NULL;
static geofence_zone_state_t s_zones[GEOFENCE_MAX_ZONES];
static bool s_initialized = false;

/* ---------------------------------------------------------------------------
 * Distance Calculation
 * ------------------------------------------------------------------------- */

/**
 * @brief Great-circle distance between two lat/lon points, in meters.
 */
static float geofence_distance_m(double lat1_deg, double lon1_deg,
                                  double lat2_deg, double lon2_deg)
{
    double lat1_rad = lat1_deg * M_PI / 180.0;
    double lat2_rad = lat2_deg * M_PI / 180.0;
    double dlat_rad = (lat2_deg - lat1_deg) * M_PI / 180.0;
    double dlon_rad = (lon2_deg - lon1_deg) * M_PI / 180.0;

    double a = sin(dlat_rad / 2.0) * sin(dlat_rad / 2.0) +
               cos(lat1_rad) * cos(lat2_rad) * sin(dlon_rad / 2.0) * sin(dlon_rad / 2.0);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));

    return (float)(EARTH_RADIUS_M * c);
}

/* ---------------------------------------------------------------------------
 * Violation Reporting
 * ------------------------------------------------------------------------- */

static void geofence_report_violation(const geofence_zone_state_t *zone,
                                       const vehicle_sample_t *sample,
                                       float distance_m, bool entered)
{
    ESP_LOGW(TAG, "Geofence violation: zone=\"%s\" event=%s distance=%.1fm",
             zone->name, entered ? "entered" : "exited", distance_m);

    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        cJSON_AddStringToObject(root, "device_id", mqtt_client_get_device_id());
        cJSON_AddNumberToObject(root, "timestamp_ms", (double)(sample->timestamp_us / 1000));
        cJSON_AddStringToObject(root, "alert_type", "geofence_violation");
        cJSON_AddStringToObject(root, "zone_name", zone->name);
        cJSON_AddStringToObject(root, "event", entered ? "entered" : "exited");
        cJSON_AddStringToObject(root, "zone_type",
                                 zone->type == GEOFENCE_ZONE_ALLOWED ? "allowed" : "restricted");
        cJSON_AddNumberToObject(root, "distance_from_center_m", distance_m);
        cJSON_AddNumberToObject(root, "latitude_deg", sample->latitude_deg);
        cJSON_AddNumberToObject(root, "longitude_deg", sample->longitude_deg);

        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str != NULL) {
            esp_err_t err = mqtt_client_publish_alert(json_str);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to enqueue geofence alert: %s", esp_err_to_name(err));
            }
            cJSON_free(json_str);
        } else {
            ESP_LOGE(TAG, "Failed to serialize geofence alert payload");
        }
        cJSON_Delete(root);
    } else {
        ESP_LOGE(TAG, "Failed to allocate cJSON object for geofence alert");
    }

    driver_event_t evt = {
        .type = DRIVER_EVENT_GEOFENCE_VIOLATION,
        .magnitude = distance_m,
        .timestamp_us = sample->timestamp_us,
    };
    esp_err_t err = driver_score_submit_event(&evt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to submit geofence event to driver_score: %s", esp_err_to_name(err));
    }
}

/* ---------------------------------------------------------------------------
 * Background Task
 * ------------------------------------------------------------------------- */

static void geofence_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Geofence task started");

    for (;;) {
        vehicle_sample_t sample;
        esp_err_t sample_err = sensor_manager_get_latest(&sample);

        if (sample_err == ESP_OK && sample.gps_fix_valid) {
            if (xSemaphoreTake(s_zones_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) == pdTRUE) {
                for (uint8_t i = 0; i < GEOFENCE_MAX_ZONES; i++) {
                    geofence_zone_state_t *zone = &s_zones[i];
                    if (!zone->in_use) {
                        continue;
                    }

                    float distance_m = geofence_distance_m(sample.latitude_deg, sample.longitude_deg,
                                                             zone->center_lat_deg, zone->center_lon_deg);
                    bool is_inside = (distance_m <= zone->radius_m);

                    if (!zone->state_known) {
                        /* Establish baseline on first observation; no
                         * event fired since we cannot know whether this
                         * represents a pre-existing violation. */
                        zone->currently_inside = is_inside;
                        zone->state_known = true;
                        continue;
                    }

                    if (is_inside != zone->currently_inside) {
                        bool is_violation =
                            (zone->type == GEOFENCE_ZONE_ALLOWED && !is_inside) ||
                            (zone->type == GEOFENCE_ZONE_RESTRICTED && is_inside);

                        if (is_violation) {
                            geofence_report_violation(zone, &sample, distance_m, is_inside);
                        } else {
                            ESP_LOGI(TAG, "Zone \"%s\": %s (non-violation transition)",
                                     zone->name, is_inside ? "entered" : "exited");
                        }
                        zone->currently_inside = is_inside;
                    }
                }
                xSemaphoreGive(s_zones_mutex);
            } else {
                ESP_LOGE(TAG, "Timed out acquiring zones mutex in geofence task");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(GEOFENCE_CHECK_PERIOD_MS));
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t geofence_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_zones_mutex = xSemaphoreCreateMutex();
    if (s_zones_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create zones mutex");
        return ESP_ERR_NO_MEM;
    }

    memset(s_zones, 0, sizeof(s_zones));

    BaseType_t task_created = xTaskCreatePinnedToCore(
        geofence_task, "geofence_task", TASK_STACK_SIZE_GEOFENCE, NULL,
        TASK_PRIORITY_GEOFENCE, NULL, TASK_CORE_PROCESSING);

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create geofence task");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Geofence module initialized (max %d zones)", GEOFENCE_MAX_ZONES);
    return ESP_OK;
}

esp_err_t geofence_add_zone(const char *name, double center_lat_deg, double center_lon_deg,
                             float radius_m, geofence_zone_type_t type, uint8_t *out_zone_index)
{
    if (name == NULL || radius_m <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_zones_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out acquiring zones mutex in geofence_add_zone");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_ERR_NO_MEM;
    for (uint8_t i = 0; i < GEOFENCE_MAX_ZONES; i++) {
        if (!s_zones[i].in_use) {
            memset(&s_zones[i], 0, sizeof(geofence_zone_state_t));
            strncpy(s_zones[i].name, name, GEOFENCE_NAME_MAX_LEN - 1);
            s_zones[i].center_lat_deg = center_lat_deg;
            s_zones[i].center_lon_deg = center_lon_deg;
            s_zones[i].radius_m = radius_m;
            s_zones[i].type = type;
            s_zones[i].in_use = true;
            s_zones[i].state_known = false;

            if (out_zone_index != NULL) {
                *out_zone_index = i;
            }

            ESP_LOGI(TAG, "Zone \"%s\" registered at index %d (radius=%.1fm, type=%s)",
                     s_zones[i].name, i, radius_m,
                     type == GEOFENCE_ZONE_ALLOWED ? "allowed" : "restricted");
            result = ESP_OK;
            break;
        }
    }

    xSemaphoreGive(s_zones_mutex);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "No free zone slots (max %d)", GEOFENCE_MAX_ZONES);
    }
    return result;
}

esp_err_t geofence_remove_zone(uint8_t zone_index)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (zone_index >= GEOFENCE_MAX_ZONES) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_zones_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out acquiring zones mutex in geofence_remove_zone");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result;
    if (s_zones[zone_index].in_use) {
        ESP_LOGI(TAG, "Zone \"%s\" (index %d) removed", s_zones[zone_index].name, zone_index);
        memset(&s_zones[zone_index], 0, sizeof(geofence_zone_state_t));
        result = ESP_OK;
    } else {
        result = ESP_ERR_INVALID_ARG;
    }

    xSemaphoreGive(s_zones_mutex);
    return result;
}
