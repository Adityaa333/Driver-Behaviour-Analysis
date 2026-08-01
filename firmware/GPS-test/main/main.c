#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

#define GPS_UART_NUM      UART_NUM_2
#define GPS_TX_PIN        GPIO_NUM_17   // ESP32 TX -> GPS RX (optional)
#define GPS_RX_PIN        GPIO_NUM_16   // ESP32 RX <- GPS TX
#define GPS_BAUD_RATE     9600
#define UART_BUF_SIZE     1024

static const char *TAG = "NEO6M";

static void gps_uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = GPS_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(GPS_UART_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GPS_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(GPS_UART_NUM, GPS_TX_PIN, GPS_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static void gps_read_task(void *arg)
{
    uint8_t *data = (uint8_t *) malloc(UART_BUF_SIZE);
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate UART buffer");
        vTaskDelete(NULL);
        return;
    }

    // Small line buffer to assemble NMEA sentences byte-by-byte
    static char line[256];
    int line_pos = 0;

    while (1) {
        int len = uart_read_bytes(GPS_UART_NUM, data, UART_BUF_SIZE, pdMS_TO_TICKS(1000));

        if (len > 0) {
            for (int i = 0; i < len; i++) {
                char c = (char)data[i];

                if (c == '\n') {
                    line[line_pos] = '\0';
                    if (line_pos > 0) {
                        printf("%s\n", line);
                    }
                    line_pos = 0;
                } else if (c != '\r') {
                    if (line_pos < (int)sizeof(line) - 1) {
                        line[line_pos++] = c;
                    } else {
                        // line too long, reset to avoid overflow
                        line_pos = 0;
                    }
                }
            }
        } else {
            // No data received in this window — helps confirm wiring issues
            ESP_LOGW(TAG, "No GPS data received (check wiring / fix?)");
        }
    }

    free(data);
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing NEO-6M GPS UART...");
    gps_uart_init();

    xTaskCreate(gps_read_task, "gps_read_task", 4096, NULL, 5, NULL);
}