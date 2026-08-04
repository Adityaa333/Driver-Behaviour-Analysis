/* ============================================================================
 * led_indicator.c
 *
 * Implementation of the GPS status LED indicator declared in
 * led_indicator.h.
 * ========================================================================= */

#include "led_indicator.h"
#include "config.h"
#include "gps.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "led_indicator";

static bool s_initialized = false;

/* ---------------------------------------------------------------------------
 * Background Task
 * ------------------------------------------------------------------------- */

static void led_indicator_task(void *arg)
{
    (void)arg;

    bool blink_state = false;

    ESP_LOGI(TAG, "LED indicator task started (gpio=%d, min_sats=%d, max_hdop=%.1f)",
             LED_STATUS_GPIO, GPS_READY_MIN_SATELLITES, GPS_READY_MAX_HDOP);

    for (;;) {
        if (gps_is_ready()) {
            gpio_set_level(LED_STATUS_GPIO, 1);
            blink_state = false; /* resync so the next "not ready" period starts lit->off */
        } else {
            blink_state = !blink_state;
            gpio_set_level(LED_STATUS_GPIO, blink_state ? 1 : 0);
        }

        vTaskDelay(pdMS_TO_TICKS(LED_CHECK_PERIOD_MS));
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t led_indicator_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_STATUS_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    gpio_set_level(LED_STATUS_GPIO, 0);

    BaseType_t task_created = xTaskCreate(
        led_indicator_task, "led_indicator_task", TASK_STACK_SIZE_LED_INDICATOR, NULL,
        TASK_PRIORITY_LED_INDICATOR, NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LED indicator task");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "LED indicator module initialized");
    return ESP_OK;
}