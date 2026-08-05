/* ============================================================================
 * main.c
 *
 * Bootstrap for the OBD-II BLE simulator. Initializes, in dependency
 * order:
 *
 *      pid_generator   - owns the live RPM/speed/coolant/throttle model
 *                         and the FreeRTOS task that advances it smoothly
 *                         over time.
 *      scenario_manager - owns the current driving scenario (Idle, City,
 *                         Highway, Aggressive, Engine Fault), the serial
 *                         console + push-button input for switching it,
 *                         and pushes new targets into pid_generator.
 *      elm327_sim      - the NimBLE GATT server itself: advertises as an
 *                         ELM327 BLE UART-bridge adapter, accepts one
 *                         connection from the DBAS firmware, and answers
 *                         AT/PID commands using values read from
 *                         pid_generator.
 *
 * This ordering matters: elm327_sim's command processor calls into
 * pid_generator on every PID request, so pid_generator must already be
 * initialized (and producing sane default values) before elm327_sim
 * starts advertising and can accept a connection.
 * ========================================================================= */

#include "esp_log.h"
#include "pid_generator.h"
#include "scenario_manager.h"
#include "elm327_sim.h"
#include "nvs_flash.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " ELM327 BLE OBD-II Simulator");
    ESP_LOGI(TAG, " (for DBAS firmware testing)");
    ESP_LOGI(TAG, "========================================");

    /* NimBLE/the BT controller want NVS for PHY calibration data and
     * (if bonding were ever used) identity/IRK storage. Without this,
     * the stack still runs, but falls back to a full RF calibration on
     * every boot and logs warnings trying to persist its local IRK -
     * cheap to avoid, same init DBAS's own wifi_manager_init() already
     * does for the same underlying reason. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase, reformatting");
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    ESP_ERROR_CHECK(pid_generator_init());
    ESP_ERROR_CHECK(scenario_manager_init());
    ESP_ERROR_CHECK(elm327_sim_init());

    ESP_LOGI(TAG, "Simulator running; advertising as BLE OBD-II adapter");
}
