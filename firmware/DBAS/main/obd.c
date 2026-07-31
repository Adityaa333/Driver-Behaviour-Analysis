// Design notes:
//
// This version replaces the Bluetooth Classic SPP transport with a BLE
// GATT client built on NimBLE, targeting a "UART-over-BLE" ELM327 clone:
// service FFF0, notify characteristic FFF1 (dongle -> us), write
// characteristic FFF2 (us -> dongle). The public contract in obd.h
// (obd_data_t, obd_init(), obd_read()) is UNCHANGED, so sensor_manager.c
// and everything downstream of it needs no modification at all.
//
// NimBLE (not Bluedroid) is used specifically for compile size: Bluedroid
// links the full Classic+BLE dual-mode stack regardless of which role you
// actually use, while NimBLE is ESP-IDF's BLE-only host. Getting the real
// flash/RAM savings also requires sdkconfig changes (BLE-only controller
// mode, NimBLE selected as the host stack, central role only, unused
// NimBLE features disabled) - see the accompanying config.h/sdkconfig
// notes; the component choice alone doesn't shrink anything.
//
// Everything here runs as a NimBLE *central/GATT-client* - this device
// never advertises or accepts inbound connections, so no GAP/GATT server
// setup (ble_svc_gap, ble_svc_gatt) is needed, which is itself a modest
// additional size/RAM saving over a general-purpose BLE template.
//
// A single background task (obd_ble_connection_task) owns the connection
// lifecycle end-to-end: connect, discover FFF0, discover FFF1/FFF2,
// discover FFF1's CCCD descriptor, subscribe, run the ELM327 AT setup
// sequence, then idle - and starts over from the top on any disconnect.
// This mirrors wifi_manager.c's and the old obd_bt_connection_task's
// "never give up, recover without a reboot" philosophy.
//
// Bytes arriving via BLE notifications on FFF1 are pushed onto a FreeRTOS
// queue by the GAP event callback (which runs in the NimBLE host task's
// context, not our own task) and consumed by whichever task is currently
// waiting on a command's response inside
// obd_read_response_until_prompt_locked(). Only one request is ever in
// flight at a time, enforced by s_bus_mutex - the same role it played in
// both earlier (CAN and SPP) versions of this file.
// ============================================================================

/* ============================================================================
 * obd.c
 *
 * Implementation of the OBD-II driver declared in obd.h.
 * ========================================================================= */

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "obd.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_uuid.h"

static const char *TAG = "obd";

#define OBD_RX_QUEUE_LEN            32     /* Notification chunks buffered */
#define OBD_RX_CHUNK_MAX_LEN        247    /* Max ATT payload after MTU exchange */
#define OBD_RESPONSE_BUF_LEN        256    /* Accumulated ASCII response */
#define OBD_AT_RETRY_COUNT          3
#define OBD_DESIRED_ATT_MTU         185    /* Comfortably fits one ELM327 line */

/* GATT layer for the "UART bridge" service exposed by the dongle
 * (see ble_scanner log: services=["1804","180F","FFF0","AE30","AE3A"],
 * FFF0 chars = FFF1[Read Notify], FFF2[Write]). 1804/180F/AE30/AE3A are
 * standard/vendor services unrelated to the OBD data path and are
 * ignored entirely - we only ever discover FFF0 by UUID. */
static const ble_uuid16_t s_svc_uuid          = BLE_UUID16_INIT(0xFFF0);
static const ble_uuid16_t s_chr_notify_uuid   = BLE_UUID16_INIT(0xFFF1);
static const ble_uuid16_t s_chr_write_uuid    = BLE_UUID16_INIT(0xFFF2);
static const ble_uuid16_t s_dsc_cccd_uuid     = BLE_UUID16_INIT(0x2902);

typedef struct {
    uint8_t data[OBD_RX_CHUNK_MAX_LEN];
    uint16_t len;
} obd_rx_chunk_t;

/* Connection/discovery state machine. Advanced only from the NimBLE
 * host task's callback context; read from obd_ble_connection_task and
 * obd_read() via the volatile qualifier rather than a mutex, since it's
 * a single word and only ever monotonically progresses forward before
 * being reset to DISCONNECTED on any failure/disconnect. */
typedef enum {
    OBD_BLE_STATE_DISCONNECTED = 0,
    OBD_BLE_STATE_CONNECTING,
    OBD_BLE_STATE_DISCOVERING,
    OBD_BLE_STATE_SUBSCRIBED,   /* GATT-ready; ELM327 AT setup not yet run */
    OBD_BLE_STATE_READY,        /* Subscribed AND ELM327 setup completed */
} obd_ble_state_t;

#define OBD_EVENT_HOST_SYNCED       (1 << 0)
#define OBD_EVENT_DISCOVERY_DONE    (1 << 1)
#define OBD_EVENT_DISCOVERY_FAILED  (1 << 2)

static volatile obd_ble_state_t s_state = OBD_BLE_STATE_DISCONNECTED;
static SemaphoreHandle_t s_bus_mutex = NULL;   /* Serializes one request at a time */
static QueueHandle_t s_rx_queue = NULL;        /* GAP notify callback -> waiting task */
static EventGroupHandle_t s_event_group = NULL;

static uint8_t s_own_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_svc_start_handle;
static uint16_t s_svc_end_handle;
static uint16_t s_notify_val_handle;
static uint16_t s_write_val_handle;
static uint16_t s_cccd_handle;

static const ble_addr_t s_target_addr = {
    .type = OBD_BLE_TARGET_ADDR_TYPE,
    .val  = OBD_BT_TARGET_MAC,
};

static bool s_initialized = false;

/* ---------------------------------------------------------------------------
 * Forward Declarations
 * ------------------------------------------------------------------------- */
static int ble_gap_event_cb(struct ble_gap_event *event, void *arg);
static void obd_ble_connection_task(void *arg);

/* ---------------------------------------------------------------------------
 * GATT Discovery Chain
 *
 * Each step kicks off the next from inside its own completion callback,
 * since NimBLE discovery is inherently async/one-shot per call. On any
 * failure, discovery is abandoned and OBD_EVENT_DISCOVERY_FAILED is set;
 * obd_ble_connection_task then tears down the connection and retries
 * from scratch rather than trying to resume a partial discovery.
 * ------------------------------------------------------------------------- */

static int on_cccd_write(uint16_t conn_handle, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)attr;
    (void)arg;

    if (error->status != 0) {
        ESP_LOGE(TAG, "CCCD write failed, status=%d", error->status);
        xEventGroupSetBits(s_event_group, OBD_EVENT_DISCOVERY_FAILED);
        return 0;
    }

    ESP_LOGI(TAG, "Subscribed to FFF1 notifications");
    s_state = OBD_BLE_STATE_SUBSCRIBED;
    xEventGroupSetBits(s_event_group, OBD_EVENT_DISCOVERY_DONE);
    return 0;
}

static int on_dsc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                        uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)arg;
    (void)chr_val_handle;

    if (error->status == 0 && dsc != NULL &&
        ble_uuid_cmp(&dsc->uuid.u, &s_dsc_cccd_uuid.u) == 0) {
        s_cccd_handle = dsc->handle;
        return 0; /* Keep discovering in case of duplicate/other descriptors */
    }

    if (error->status == BLE_HS_EDONE) {
        if (s_cccd_handle == 0) {
            ESP_LOGE(TAG, "FFF1 has no CCCD (0x2902) descriptor - cannot subscribe");
            xEventGroupSetBits(s_event_group, OBD_EVENT_DISCOVERY_FAILED);
            return 0;
        }
        /* Notifications (not indications): write 0x0001 little-endian. */
        uint8_t notify_on[2] = { 0x01, 0x00 };
        int rc = ble_gattc_write_flat(conn_handle, s_cccd_handle,
                                       notify_on, sizeof(notify_on),
                                       on_cccd_write, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "ble_gattc_write_flat(CCCD) failed, rc=%d", rc);
            xEventGroupSetBits(s_event_group, OBD_EVENT_DISCOVERY_FAILED);
        }
        return 0;
    }

    if (error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGE(TAG, "Descriptor discovery failed, status=%d", error->status);
        xEventGroupSetBits(s_event_group, OBD_EVENT_DISCOVERY_FAILED);
    }
    return 0;
}

static int on_chr_write_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                              const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;

    if (error->status != 0 || chr == NULL) {
        ESP_LOGE(TAG, "FFF2 (write) characteristic not found, status=%d", error->status);
        xEventGroupSetBits(s_event_group, OBD_EVENT_DISCOVERY_FAILED);
        return 0;
    }

    s_write_val_handle = chr->val_handle;
    ESP_LOGI(TAG, "Discovered FFF2 (write) at handle %d", s_write_val_handle);

    /* Next: find FFF1's CCCD descriptor so we can subscribe. Descriptors
     * live between FFF1's value handle and the end of the service. */
    int rc = ble_gattc_disc_all_dscs(conn_handle, s_notify_val_handle + 1,
                                      s_svc_end_handle, on_dsc_disc, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_disc_all_dscs failed, rc=%d", rc);
        xEventGroupSetBits(s_event_group, OBD_EVENT_DISCOVERY_FAILED);
    }
    return 0;
}

static int on_chr_notify_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                               const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;

    if (error->status != 0 || chr == NULL) {
        ESP_LOGE(TAG, "FFF1 (notify) characteristic not found, status=%d", error->status);
        xEventGroupSetBits(s_event_group, OBD_EVENT_DISCOVERY_FAILED);
        return 0;
    }

    s_notify_val_handle = chr->val_handle;
    ESP_LOGI(TAG, "Discovered FFF1 (notify) at handle %d", s_notify_val_handle);

    /* Next: FFF2 (write). Searching the whole service range again is
     * fine - NimBLE only returns handles inside [start,end]. */
    int rc = ble_gattc_disc_chrs_by_uuid(conn_handle, s_svc_start_handle, s_svc_end_handle,
                                          &s_chr_write_uuid.u, on_chr_write_disc, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_disc_chrs_by_uuid(FFF2) failed, rc=%d", rc);
        xEventGroupSetBits(s_event_group, OBD_EVENT_DISCOVERY_FAILED);
    }
    return 0;
}

static int on_svc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                        const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;

    if (error->status != 0 || service == NULL) {
        ESP_LOGE(TAG, "FFF0 service not found, status=%d", error->status);
        xEventGroupSetBits(s_event_group, OBD_EVENT_DISCOVERY_FAILED);
        return 0;
    }

    s_svc_start_handle = service->start_handle;
    s_svc_end_handle = service->end_handle;
    s_cccd_handle = 0;
    ESP_LOGI(TAG, "Discovered FFF0 service, handles [%d,%d]",
             s_svc_start_handle, s_svc_end_handle);

    int rc = ble_gattc_disc_chrs_by_uuid(conn_handle, s_svc_start_handle, s_svc_end_handle,
                                          &s_chr_notify_uuid.u, on_chr_notify_disc, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_disc_chrs_by_uuid(FFF1) failed, rc=%d", rc);
        xEventGroupSetBits(s_event_group, OBD_EVENT_DISCOVERY_FAILED);
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * GAP Event Handling (connect/disconnect/notify/MTU)
 *
 * NimBLE delivers incoming notifications through this same GAP callback
 * (BLE_GAP_EVENT_NOTIFY_RX) rather than a separate GATT subscription
 * callback - there is no dedicated "data indication" callback to
 * register, unlike the SPP version's ESP_SPP_DATA_IND_EVT.
 * ------------------------------------------------------------------------- */

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) {
                ESP_LOGW(TAG, "Connection attempt failed, status=%d", event->connect.status);
                s_state = OBD_BLE_STATE_DISCONNECTED;
                xEventGroupSetBits(s_event_group, OBD_EVENT_DISCOVERY_FAILED);
                return 0;
            }

            ESP_LOGI(TAG, "Connected to OBD dongle, conn_handle=%d", event->connect.conn_handle);
            s_conn_handle = event->connect.conn_handle;
            s_state = OBD_BLE_STATE_DISCOVERING;

            /* Best-effort: a larger ATT MTU means fewer notification
             * fragments per ELM327 response line. obd_read_response_
             * until_prompt_locked() reassembles fragments regardless, so
             * this is a latency/efficiency optimization, not a
             * correctness requirement - no need to wait for its result
             * before starting service discovery. */
            ble_gattc_exchange_mtu(s_conn_handle, NULL, NULL);

            {
                int rc = ble_gattc_disc_svc_by_uuid(s_conn_handle, &s_svc_uuid.u,
                                                     on_svc_disc, NULL);
                if (rc != 0) {
                    ESP_LOGE(TAG, "ble_gattc_disc_svc_by_uuid failed, rc=%d", rc);
                    xEventGroupSetBits(s_event_group, OBD_EVENT_DISCOVERY_FAILED);
                }
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGW(TAG, "Disconnected from OBD dongle, reason=%d",
                     event->disconnect.reason);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_state = OBD_BLE_STATE_DISCONNECTED;
            /* Unblock anything waiting in obd_read_response_until_prompt_
             * locked() rather than leaving it to time out. */
            xEventGroupSetBits(s_event_group, OBD_EVENT_DISCOVERY_FAILED);
            return 0;

        case BLE_GAP_EVENT_NOTIFY_RX: {
            /* Runs in the NimBLE host task's context - keep this fast
             * and non-blocking; just hand the bytes off to whichever
             * task is waiting inside obd_read_response_until_prompt_
             * locked(). */
            if (event->notify_rx.attr_handle != s_notify_val_handle) {
                return 0; /* Not our characteristic (shouldn't happen) */
            }

            obd_rx_chunk_t chunk;
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            if (len > OBD_RX_CHUNK_MAX_LEN) {
                len = OBD_RX_CHUNK_MAX_LEN;
            }
            os_mbuf_copydata(event->notify_rx.om, 0, len, chunk.data);
            chunk.len = len;

            if (xQueueSend(s_rx_queue, &chunk, 0) != pdTRUE) {
                ESP_LOGW(TAG, "OBD RX queue full, dropping notification chunk");
            }
            return 0;
        }

        default:
            return 0;
    }
}

/* ---------------------------------------------------------------------------
 * SPP-era response reading logic, adapted to pull from BLE notification
 * chunks instead of SPP data-indication chunks. Framing (ASCII, '\r'
 * terminated commands, '>' terminated responses) is identical since the
 * dongle firmware's ELM327 emulation doesn't change with transport.
 * ------------------------------------------------------------------------- */

static esp_err_t obd_read_response_until_prompt_locked(char *out_buf, size_t out_buf_len,
                                                        uint32_t timeout_ms)
{
    size_t written = 0;
    out_buf[0] = '\0';

    int64_t deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000);

    for (;;) {
        if (s_state != OBD_BLE_STATE_READY) {
            return ESP_ERR_INVALID_STATE; /* Disconnected mid-request */
        }

        int64_t remaining_ms = (deadline_us - esp_timer_get_time()) / 1000;
        if (remaining_ms <= 0) {
            ESP_LOGW(TAG, "Timed out waiting for ELM327 prompt");
            return ESP_ERR_TIMEOUT;
        }

        obd_rx_chunk_t chunk;
        if (xQueueReceive(s_rx_queue, &chunk, pdMS_TO_TICKS(remaining_ms)) != pdTRUE) {
            continue; /* Loop re-checks the overall deadline/connection state */
        }

        for (uint16_t i = 0; i < chunk.len && written < (out_buf_len - 1); i++) {
            out_buf[written++] = (char)chunk.data[i];
            if (chunk.data[i] == '>') {
                out_buf[written] = '\0';
                return ESP_OK;
            }
        }
    }
}

static esp_err_t obd_send_command_locked(const char *cmd, char *resp_buf, size_t resp_buf_len,
                                          uint32_t timeout_ms)
{
    if (s_state != OBD_BLE_STATE_READY) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Drain any stale bytes left over from a previous exchange before
     * sending a new command, so they can't be misread as part of this
     * command's response. */
    obd_rx_chunk_t stale;
    while (xQueueReceive(s_rx_queue, &stale, 0) == pdTRUE) {
        /* discard */
    }

    int rc = ble_gattc_write_no_rsp_flat(s_conn_handle, s_write_val_handle,
                                          cmd, (uint16_t)strlen(cmd));
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gattc_write_no_rsp_flat failed for command \"%s\", rc=%d", cmd, rc);
        return ESP_ERR_TIMEOUT;
    }

    return obd_read_response_until_prompt_locked(resp_buf, resp_buf_len, timeout_ms);
}

/* ---------------------------------------------------------------------------
 * ELM327 Setup (AT Commands) - unchanged from the SPP version; the
 * dongle's ELM327 emulation is transport-agnostic.
 * ------------------------------------------------------------------------- */

static esp_err_t obd_elm327_configure_locked(void)
{
    static const char *const setup_cmds[] = {
        "ATZ\r",    /* Reset */
        "ATE0\r",   /* Echo off - avoids re-parsing our own command text */
        "ATL0\r",   /* Linefeeds off - simplifies response parsing */
        "ATSP0\r",  /* Auto-select OBD-II protocol */
    };

    char resp[OBD_RESPONSE_BUF_LEN];

    for (size_t i = 0; i < sizeof(setup_cmds) / sizeof(setup_cmds[0]); i++) {
        esp_err_t err = ESP_ERR_TIMEOUT;
        for (int attempt = 1; attempt <= OBD_AT_RETRY_COUNT && err != ESP_OK; attempt++) {
            err = obd_send_command_locked(setup_cmds[i], resp, sizeof(resp),
                                           OBD_REQUEST_TIMEOUT_MS);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ELM327 setup command \"%s\" failed", setup_cmds[i]);
            return err;
        }
        /* ATZ reboots the adapter's own microcontroller; give it a beat
         * before sending the next command. */
        if (i == 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    ESP_LOGI(TAG, "ELM327 adapter configured");
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * PID Request / Response Parsing - byte-for-byte unchanged from the SPP
 * version; ELM327 Mode 01 response framing doesn't depend on transport.
 * ------------------------------------------------------------------------- */

static bool obd_parse_pid_response(const char *resp, uint8_t pid,
                                    uint8_t *out_byte_a, uint8_t *out_byte_b,
                                    bool *out_has_byte_b)
{
    char work_buf[OBD_RESPONSE_BUF_LEN];
    strncpy(work_buf, resp, sizeof(work_buf) - 1);
    work_buf[sizeof(work_buf) - 1] = '\0';

    char *saveptr = NULL;
    char *tok = strtok_r(work_buf, " \r\n", &saveptr);

    uint8_t bytes[8];
    int byte_count = 0;
    bool found_header = false;

    while (tok != NULL) {
        /* Skip non-hex noise such as "SEARCHING..." or the trailing '>'. */
        if (strlen(tok) == 2 && isxdigit((unsigned char)tok[0]) && isxdigit((unsigned char)tok[1])) {
            uint8_t val = (uint8_t)strtol(tok, NULL, 16);

            if (!found_header) {
                if (val == 0x41) {
                    found_header = true; /* Mode 01 response marker */
                }
            } else if (byte_count == 0 && val != pid) {
                /* First byte after 0x41 must echo the requested PID;
                 * otherwise this is a response to something else. */
                found_header = false;
            } else {
                if (byte_count < (int)sizeof(bytes)) {
                    bytes[byte_count++] = val;
                }
            }
        }
        tok = strtok_r(NULL, " \r\n", &saveptr);
    }

    /* bytes[0] is the echoed PID itself; data starts at bytes[1]. */
    if (!found_header || byte_count < 2) {
        return false;
    }

    *out_byte_a = bytes[1];
    if (byte_count >= 3) {
        *out_byte_b = bytes[2];
        *out_has_byte_b = true;
    } else {
        *out_has_byte_b = false;
    }
    return true;
}

static esp_err_t obd_request_pid_locked(uint8_t pid, uint8_t *out_byte_a,
                                         uint8_t *out_byte_b, bool *out_has_byte_b)
{
    if (s_state != OBD_BLE_STATE_READY) {
        return ESP_ERR_INVALID_STATE;
    }

    char cmd[8];
    snprintf(cmd, sizeof(cmd), "01%02X\r", pid);

    char resp[OBD_RESPONSE_BUF_LEN];
    esp_err_t err = obd_send_command_locked(cmd, resp, sizeof(resp), OBD_REQUEST_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Timed out waiting for response to PID 0x%02X", pid);
        return ESP_ERR_TIMEOUT;
    }

    if (!obd_parse_pid_response(resp, pid, out_byte_a, out_byte_b, out_has_byte_b)) {
        ESP_LOGW(TAG, "Unparseable/negative response for PID 0x%02X: \"%s\"", pid, resp);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * NimBLE Host Callbacks
 * ------------------------------------------------------------------------- */

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);
}

static void ble_on_sync(void)
{
    /* Prefer a public address; ble_hs_id_infer_auto() falls back to
     * whatever address type the controller actually has available. */
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed, rc=%d", rc);
        return;
    }
    xEventGroupSetBits(s_event_group, OBD_EVENT_HOST_SYNCED);
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run(); /* Returns only on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* ---------------------------------------------------------------------------
 * Connection Management Task
 *
 * Owns the complete BLE connection lifecycle:
 *
 *      Connect
 *          ↓
 *      Discover FFF0 Service
 *          ↓
 *      Discover FFF1 (Notify)
 *          ↓
 *      Discover FFF2 (Write)
 *          ↓
 *      Discover CCCD
 *          ↓
 *      Enable Notifications
 *          ↓
 *      Run ELM327 AT Setup
 *          ↓
 *      READY
 *
 * The task itself never performs service or characteristic discovery.
 * Instead, it initiates the BLE connection and then waits for the
 * asynchronous GAP/GATT callback chain to either complete successfully or
 * report a failure. This separation keeps all BLE protocol handling inside
 * the NimBLE host callbacks while this task simply manages the overall
 * connection state machine.
 *
 * Any failure during connection, GATT discovery, notification
 * subscription, AT-command initialization, or a later disconnect causes
 * the driver to return to DISCONNECTED and restart the entire sequence
 * after a short delay, mirroring the reconnect philosophy used by
 * wifi_manager.c and the previous Bluetooth Classic implementation.
 * ------------------------------------------------------------------------- */

static void obd_ble_connection_task(void *arg)
{
    (void)arg;

    /* Wait until the NimBLE host has finished initialization and selected
     * our own Bluetooth address. No connection attempts are made before
     * the host reports it is fully synchronized. */
    xEventGroupWaitBits(s_event_group,
                        OBD_EVENT_HOST_SYNCED,
                        pdFALSE,
                        pdTRUE,
                        portMAX_DELAY);

    for (;;) {

        /* Clear any stale completion flags left from a previous
         * connection attempt before starting a fresh connection cycle. */
        xEventGroupClearBits(s_event_group,
                             OBD_EVENT_DISCOVERY_DONE |
                             OBD_EVENT_DISCOVERY_FAILED);

        ESP_LOGI(TAG, "Attempting BLE connection to OBD dongle");
        s_state = OBD_BLE_STATE_CONNECTING;

        /* Connection parameters are intentionally conservative since the
         * data rate required for ELM327 PID polling is very low. */
        struct ble_gap_conn_params conn_params = { 0 };
        conn_params.scan_itvl = 0x0010;
        conn_params.scan_window = 0x0010;
        conn_params.itvl_min = BLE_GAP_INITIAL_CONN_ITVL_MIN;
        conn_params.itvl_max = BLE_GAP_INITIAL_CONN_ITVL_MAX;
        conn_params.latency = 0;
        conn_params.supervision_timeout = 0x0100;
        conn_params.min_ce_len = 0x0010;
        conn_params.max_ce_len = 0x0300;

        /* Begin an asynchronous BLE connection attempt.
         *
         * A successful return from ble_gap_connect() does NOT mean that we
         * are connected. It only means the controller accepted the request
         * to begin connecting. The remainder of the process happens inside
         * ble_gap_event_cb():
         *
         *      CONNECT
         *          ↓
         *      MTU Exchange
         *          ↓
         *      Discover FFF0
         *          ↓
         *      Discover FFF1
         *          ↓
         *      Discover FFF2
         *          ↓
         *      Discover CCCD
         *          ↓
         *      Enable Notifications
         *
         * If the connection cannot even be started (controller busy,
         * invalid parameters, etc.), simply wait a short period and retry
         * the entire connection sequence.
         */
        int rc = ble_gap_connect(s_own_addr_type,
                                 &s_target_addr,
                                 OBD_BT_CONNECT_TIMEOUT_MS,
                                 &conn_params,
                                 ble_gap_event_cb,
                                 NULL);

        if (rc != 0) {
            ESP_LOGW(TAG,
                     "ble_gap_connect failed to start, rc=%d",
                     rc);

            s_state = OBD_BLE_STATE_DISCONNECTED;
            vTaskDelay(pdMS_TO_TICKS(OBD_BT_RECONNECT_DELAY_MS));
            continue;
        }

        /* Connection establishment and the complete GATT discovery chain
         * execute asynchronously inside the GAP/GATT callbacks. Rather
         * than polling intermediate state, simply wait until one of two
         * terminal events occurs:
         *
         *      OBD_EVENT_DISCOVERY_DONE
         *          Complete connection + discovery + notification
         *          subscription succeeded.
         *
         *      OBD_EVENT_DISCOVERY_FAILED
         *          Connection failed, discovery failed, subscription
         *          failed, or the peer disconnected before initialization
         *          completed.
         *
         * The timeout intentionally exceeds the BLE connection timeout to
         * provide sufficient time for the full multi-stage discovery
         * process.
         */
        EventBits_t bits = xEventGroupWaitBits(
            s_event_group,
            OBD_EVENT_DISCOVERY_DONE |
            OBD_EVENT_DISCOVERY_FAILED,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(OBD_BT_CONNECT_TIMEOUT_MS + 10000));

        if (!(bits & OBD_EVENT_DISCOVERY_DONE)) {

            ESP_LOGW(TAG,
                     "Connect/discovery did not complete, retrying in %d ms",
                     OBD_BT_RECONNECT_DELAY_MS);

            /* If a partial connection still exists, explicitly terminate
             * it before restarting the connection sequence. */
            if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                ble_gap_terminate(s_conn_handle,
                                  BLE_ERR_REM_USER_CONN_TERM);
            }

            s_state = OBD_BLE_STATE_DISCONNECTED;
            vTaskDelay(pdMS_TO_TICKS(OBD_BT_RECONNECT_DELAY_MS));
            continue;
        }

        /* The BLE transport is now operational and notifications are
         * enabled. Perform the ELM327 initialization sequence (ATZ,
         * ATE0, ATL0, ATSP0) before allowing normal PID requests. */
        if (xSemaphoreTake(s_bus_mutex,
                           pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) == pdTRUE) {

            esp_err_t err = obd_elm327_configure_locked();

            if (err == ESP_OK) {
                s_state = OBD_BLE_STATE_READY;
                ESP_LOGI(TAG, "OBD dongle ready");
            } else {

                ESP_LOGW(TAG,
                         "ELM327 setup failed, disconnecting and retrying");

                if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                    ble_gap_terminate(s_conn_handle,
                                      BLE_ERR_REM_USER_CONN_TERM);
                }

                s_state = OBD_BLE_STATE_DISCONNECTED;
            }

            xSemaphoreGive(s_bus_mutex);
        }

        /* Idle while the connection remains healthy. If the peer
         * disconnects, BLE_GAP_EVENT_DISCONNECT asynchronously resets
         * s_state to DISCONNECTED, causing this loop to exit and begin a
         * fresh connection cycle automatically. */
        while (s_state == OBD_BLE_STATE_READY) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t obd_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_bus_mutex = xSemaphoreCreateMutex();
    if (s_bus_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create OBD bus mutex");
        return ESP_ERR_NO_MEM;
    }

    s_rx_queue = xQueueCreate(OBD_RX_QUEUE_LEN, sizeof(obd_rx_chunk_t));
    if (s_rx_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create OBD RX queue");
        return ESP_ERR_NO_MEM;
    }

    s_event_group = xEventGroupCreate();
    if (s_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create OBD event group");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    /* Central/GATT-client role only - no store config, no GAP/GATT
     * server callbacks, keeping the linked-in surface minimal. */

    nimble_port_freertos_init(ble_host_task);

    BaseType_t task_created = xTaskCreatePinnedToCore(
        obd_ble_connection_task, "obd_ble_conn_task", TASK_STACK_SIZE_OBD, NULL,
        TASK_PRIORITY_OBD, NULL, TASK_CORE_SENSOR_ACQUISITION);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OBD BLE connection task");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "OBD-II BLE (NimBLE/ELM327 UART-bridge) driver initialized");
    return ESP_OK;
}

esp_err_t obd_read(obd_data_t *data)
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state != OBD_BLE_STATE_READY) {
        /* Not connected/subscribed/configured right now - same "report
         * honestly, don't hold stale data" behavior as earlier versions;
         * sensor_manager's OBD task clears its cache on this. */
        return ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(MUTEX_MAX_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out acquiring OBD bus mutex");
        return ESP_ERR_TIMEOUT;
    }

    memset(data, 0, sizeof(obd_data_t));
    uint8_t byte_a = 0, byte_b = 0;
    bool has_byte_b = false;
    uint8_t valid_count = 0;

    /* Engine RPM: ((A * 256) + B) / 4 */
    if (obd_request_pid_locked(OBD_PID_ENGINE_RPM, &byte_a, &byte_b, &has_byte_b) == ESP_OK
        && has_byte_b) {
        data->engine_rpm = (uint16_t)(((uint16_t)byte_a * 256 + byte_b) / 4);
        data->engine_rpm_valid = true;
        valid_count++;
    }

    /* Vehicle speed: A, already in km/h */
    if (obd_request_pid_locked(OBD_PID_VEHICLE_SPEED, &byte_a, &byte_b, &has_byte_b) == ESP_OK) {
        data->vehicle_speed_kmh = (float)byte_a;
        data->vehicle_speed_valid = true;
        valid_count++;
    }

    /* Throttle position: A * 100 / 255, percent */
    if (obd_request_pid_locked(OBD_PID_THROTTLE_POSITION, &byte_a, &byte_b, &has_byte_b) == ESP_OK) {
        data->throttle_position_pct = ((float)byte_a * 100.0f) / 255.0f;
        data->throttle_position_valid = true;
        valid_count++;
    }

    /* Coolant temperature: A - 40, Celsius */
    if (obd_request_pid_locked(OBD_PID_ENGINE_COOLANT_TEMP, &byte_a, &byte_b, &has_byte_b) == ESP_OK) {
        data->coolant_temp_c = (int16_t)byte_a - 40;
        data->coolant_temp_valid = true;
        valid_count++;
    }

    xSemaphoreGive(s_bus_mutex);

    data->timestamp_us = esp_timer_get_time();

    if (valid_count == 0) {
        ESP_LOGW(TAG, "No OBD-II PIDs responded this cycle (dongle disconnected/ignition off?)");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}
