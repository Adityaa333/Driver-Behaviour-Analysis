/* ============================================================================
 * elm327_sim.c
 *
 * Implementation of the ELM327 BLE simulator declared in elm327_sim.h.
 *
 * ----------------------------------------------------------------------
 * INTEGRATION NOTE - read this before wiring up hardware:
 *
 * DBAS's obd.c is a NimBLE GATT *client* that connects with
 * ble_gap_connect() to a hardcoded peer address (OBD_BT_TARGET_ADDR_TYPE
 * / OBD_BT_TARGET_MAC in DBAS's config.h) - it never scans by name or
 * advertised service UUID. Since we are not allowed to modify DBAS
 * firmware, this simulator must present that exact address as its own
 * BLE identity, or DBAS's direct connection attempt will simply time
 * out against a peer that never responds. ELM327_SIM_TARGET_MAC below
 * MUST be kept identical to DBAS's config.h OBD_BT_TARGET_MAC at all
 * times - copy-paste it any time one changes. This is the one piece of
 * "configuration shared between two independent firmware projects" that
 * cannot be avoided, since the alternative would be editing DBAS's
 * config.h, which was explicitly ruled out.
 *
 * IMPORTANT MECHANISM NOTE: there is no NimBLE *host*-level API to set
 * the public address (an earlier version of this file incorrectly
 * assumed one existed, ble_hs_id_set_pub() - it does not; NimBLE expects
 * the public address to already be correct, sourced from the
 * controller, by the time the host syncs). The override therefore
 * happens one layer down, in elm327_sim_init() via
 * esp_iface_mac_addr_set(..., ESP_MAC_BT), BEFORE nimble_port_init()
 * brings the controller up. See that function for details.
 * ----------------------------------------------------------------------
 *
 * Architecture:
 *   - GATT write characteristic (0xFFF2) access callback runs in the
 *     NimBLE host task's context. It only accumulates incoming bytes
 *     into a command buffer and, once a complete '\r'-terminated ELM327
 *     command is seen, pushes the command string onto a queue and
 *     returns immediately - it never blocks on PID generation or
 *     notification sends, keeping the NimBLE host task responsive.
 *   - A dedicated elm327_cmd_task drains that queue, parses each
 *     command (AT command or Mode 01 PID request), builds the ELM327-
 *     style ASCII response (pulling live values from pid_generator for
 *     PID requests), and sends it back via notify on 0xFFF1, chunked to
 *     fit the negotiated ATT MTU exactly like a real adapter would.
 * This mirrors DBAS's own obd.c split between the GAP callback (fast,
 * non-blocking) and its connection-management task.
 * ========================================================================= */

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include "elm327_sim.h"
#include "pid_generator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "elm327_sim";

/* ---------------------------------------------------------------------------
 * BLE Identity / Advertising Configuration
 * ------------------------------------------------------------------------- */

/* MUST match DBAS's config.h OBD_BT_TARGET_MAC exactly - see the
 * integration note above. DBAS's config.h currently has:
 *   #define OBD_BT_TARGET_MAC  { 0x96, 0x02, 0x00, 0x11, 0x1E, 0x66 }
 *   #define OBD_BLE_TARGET_ADDR_TYPE BLE_ADDR_PUBLIC
 */
static const uint8_t ELM327_SIM_TARGET_MAC[6] = { 0x96, 0x02, 0x00, 0x11, 0x1E, 0x66 };

/* Advertised name. Real cheap ELM327 BLE ("UART bridge") clones use a
 * variety of names depending on manufacturer (e.g. "OBDII", "Vlink",
 * "V-LINK"); DBAS connects by address, not name, so this is cosmetic -
 * it only affects what a phone BLE scanner app would show. Pick
 * whichever your real adapter used, if you're replacing a specific one. */
#define ELM327_SIM_DEVICE_NAME      "OBDII"

#define ELM327_SIM_SVC_UUID         0xFFF0
#define ELM327_SIM_CHR_NOTIFY_UUID  0xFFF1   /* adapter -> DBAS (notify) */
#define ELM327_SIM_CHR_WRITE_UUID   0xFFF2   /* DBAS -> adapter (write)  */

/* ---------------------------------------------------------------------------
 * Command Buffering / Queue
 * ------------------------------------------------------------------------- */

#define ELM327_CMD_MAX_LEN          32    /* Longest real command is short */
#define ELM327_CMD_QUEUE_LEN        5
#define ELM327_RESP_MAX_LEN         128   /* Longest response we generate */
#define ELM327_DEFAULT_ATT_MTU      23    /* Pre-negotiation ATT default */

typedef struct {
    char text[ELM327_CMD_MAX_LEN];
} elm327_cmd_msg_t;

static QueueHandle_t s_cmd_queue = NULL;

/* Accumulation buffer for partial writes - only ever touched from the
 * NimBLE host task's context (the GATT access callback), so no mutex
 * is required. */
static char s_rx_accum[ELM327_CMD_MAX_LEN];
static size_t s_rx_accum_len = 0;

/* ---------------------------------------------------------------------------
 * Connection / Notification State
 * ------------------------------------------------------------------------- */

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_notify_val_handle = 0;   /* Filled in by NimBLE at registration */
static bool s_notify_enabled = false;      /* True once DBAS writes the CCCD */
static uint16_t s_att_mtu = ELM327_DEFAULT_ATT_MTU;

/* ---------------------------------------------------------------------------
 * Emulated Adapter State (AT command settings)
 * ------------------------------------------------------------------------- */

static bool s_echo_enabled = true;      /* Real ELM327 default: echo ON  */
static bool s_headers_enabled = false;  /* Real ELM327 default: headers OFF */
static bool s_linefeeds_enabled = true; /* Real ELM327 default: linefeeds ON */
static bool s_search_reported = false;  /* "SEARCHING..." fires once after ATSP0/reset */

static uint8_t s_own_addr_type;

/* ---------------------------------------------------------------------------
 * GATT Access Callbacks (forward declarations needed by the service table)
 * ------------------------------------------------------------------------- */

static int elm327_gatt_notify_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                         struct ble_gatt_access_ctxt *ctxt, void *arg);
static int elm327_gatt_write_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                        struct ble_gatt_access_ctxt *ctxt, void *arg);
static int elm327_gap_event_cb(struct ble_gap_event *event, void *arg);

/* ---------------------------------------------------------------------------
 * GATT Service Table
 *
 * Single primary service (0xFFF0) with two characteristics, matching
 * the "UART bridge" layout DBAS's obd.c already expects:
 *   - 0xFFF1: NOTIFY only. NimBLE auto-adds a CCCD (0x2902) descriptor
 *     for any characteristic with the NOTIFY flag, so DBAS's descriptor
 *     discovery in obd.c will find one here (unlike some cheap real
 *     clones that omit it - see obd.c's on_dsc_disc() fallback comment
 *     for why that path exists; this simulator exercises the "normal"
 *     path instead).
 *   - 0xFFF2: WRITE_NO_RSP, matching DBAS's ble_gattc_write_flat() usage
 *     (a write-without-response, acknowledged at the ATT layer only).
 * ------------------------------------------------------------------------- */

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(ELM327_SIM_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(ELM327_SIM_CHR_NOTIFY_UUID),
                .access_cb = elm327_gatt_notify_access_cb,
                .val_handle = &s_notify_val_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = BLE_UUID16_DECLARE(ELM327_SIM_CHR_WRITE_UUID),
                .access_cb = elm327_gatt_write_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            { 0 }, /* Terminator */
        },
    },
    { 0 }, /* Terminator */
};

/* ---------------------------------------------------------------------------
 * GATT Access Callbacks
 * ------------------------------------------------------------------------- */

/**
 * @brief Access callback for the notify characteristic. We only ever
 *        push data via ble_gatts_notify_custom(), never in response to
 *        a read/write on this handle - NimBLE still requires an
 *        access_cb to be registered, so this exists purely to satisfy
 *        that and should never actually be invoked by a well-behaved
 *        peer.
 */
static int elm327_gatt_notify_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    ESP_LOGW(TAG, "Unexpected direct access to notify characteristic");
    return BLE_ATT_ERR_UNLIKELY;
}

/**
 * @brief Access callback for the write characteristic. Appends incoming
 *        bytes to the accumulation buffer; once a '\r' (ELM327 command
 *        terminator) is seen, enqueues the complete command for
 *        elm327_cmd_task and resets the buffer.
 *
 * Runs in the NimBLE host task's context - must not block.
 */
static int elm327_gatt_write_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    uint8_t chunk[ELM327_CMD_MAX_LEN];
    if (len > sizeof(chunk)) {
        len = sizeof(chunk);
    }
    os_mbuf_copydata(ctxt->om, 0, len, chunk);

    for (uint16_t i = 0; i < len; i++) {
        char c = (char)chunk[i];

        if (c == '\r' || c == '\n') {
            if (s_rx_accum_len > 0) {
                elm327_cmd_msg_t msg;
                memset(&msg, 0, sizeof(msg));
                memcpy(msg.text, s_rx_accum, s_rx_accum_len);
                msg.text[s_rx_accum_len] = '\0';
                s_rx_accum_len = 0;

                if (xQueueSend(s_cmd_queue, &msg, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "Command queue full, dropping \"%s\"", msg.text);
                }
            }
            continue;
        }

        if (s_rx_accum_len < (sizeof(s_rx_accum) - 1)) {
            s_rx_accum[s_rx_accum_len++] = c;
        } else {
            /* Command exceeded expected length; discard and resync on
             * the next terminator, mirroring gps.c's overflow handling
             * for the same class of problem (untrusted line-oriented
             * input over a byte stream). */
            ESP_LOGW(TAG, "Command exceeded buffer size, discarding");
            s_rx_accum_len = 0;
        }
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * Response Transmission
 * ------------------------------------------------------------------------- */

/**
 * @brief Send a complete ELM327 response, chunked to fit the negotiated
 *        ATT MTU, via notify on 0xFFF1. A real adapter's UART-to-BLE
 *        bridge firmware does the same fragmentation; DBAS's obd.c
 *        already reassembles fragmented responses in
 *        obd_read_response_until_prompt_locked(), so this is required
 *        for realism, not merely convenience.
 */
static void elm327_send_response(const char *response, size_t len)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_notify_enabled) {
        ESP_LOGW(TAG, "Cannot send response - not connected/subscribed");
        return;
    }

    /* 3 bytes of ATT overhead (opcode + handle) per notification. */
    size_t max_chunk = (s_att_mtu > 3) ? (size_t)(s_att_mtu - 3) : 20;

    size_t offset = 0;
    while (offset < len) {
        size_t chunk_len = len - offset;
        if (chunk_len > max_chunk) {
            chunk_len = max_chunk;
        }

        struct os_mbuf *om = ble_hs_mbuf_from_flat(response + offset, chunk_len);
        if (om == NULL) {
            ESP_LOGE(TAG, "Failed to allocate mbuf for notification chunk");
            return;
        }

        int rc = ble_gatts_notify_custom(s_conn_handle, s_notify_val_handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "ble_gatts_notify_custom failed, rc=%d", rc);
            return;
        }

        offset += chunk_len;
    }
}

/**
 * @brief Convenience wrapper: format into a stack buffer and send.
 */
static void elm327_respond(const char *fmt, ...)
{
    char buf[ELM327_RESP_MAX_LEN];
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (written < 0) {
        ESP_LOGE(TAG, "Response formatting failed");
        return;
    }
    if ((size_t)written >= sizeof(buf)) {
        written = sizeof(buf) - 1; /* Truncated; still send what fit */
    }

    ESP_LOGD(TAG, "TX <- \"%s\"", buf);
    elm327_send_response(buf, (size_t)written);
}

/* ---------------------------------------------------------------------------
 * Command Parsing / Dispatch
 * ------------------------------------------------------------------------- */

/**
 * @brief Case-insensitive prefix match, since real ELM327 command
 *        parsers accept commands in either case.
 */
static bool elm327_cmd_is(const char *cmd, const char *pattern)
{
    return strcasecmp(cmd, pattern) == 0;
}

/**
 * @brief Handle a fully-received AT command. Response format mirrors a
 *        real ELM327's line-oriented ASCII replies.
 */
static void elm327_handle_at_command(const char *cmd)
{
    if (elm327_cmd_is(cmd, "ATZ")) {
        s_echo_enabled = true;
        s_headers_enabled = false;
        s_linefeeds_enabled = true;
        s_search_reported = false;
        /* Real adapters take a brief moment to "reboot" here; DBAS's
         * obd_elm327_configure_locked() already waits 500ms after ATZ
         * before its next command, so we don't need an artificial delay
         * on this side as well. */
        elm327_respond("ELM327 v1.5\r\r>");
        return;
    }
    if (elm327_cmd_is(cmd, "ATE0")) {
        s_echo_enabled = false;
        elm327_respond("OK\r\r>");
        return;
    }
    if (elm327_cmd_is(cmd, "ATE1")) {
        s_echo_enabled = true;
        elm327_respond("OK\r\r>");
        return;
    }
    if (elm327_cmd_is(cmd, "ATL0")) {
        s_linefeeds_enabled = false;
        elm327_respond("OK\r\r>");
        return;
    }
    if (elm327_cmd_is(cmd, "ATL1")) {
        s_linefeeds_enabled = true;
        elm327_respond("OK\r\r>");
        return;
    }
    if (elm327_cmd_is(cmd, "ATH0")) {
        s_headers_enabled = false;
        elm327_respond("OK\r\r>");
        return;
    }
    if (elm327_cmd_is(cmd, "ATH1")) {
        s_headers_enabled = true;
        elm327_respond("OK\r\r>");
        return;
    }
    if (elm327_cmd_is(cmd, "ATSP0")) {
        /* Auto protocol select. Real adapters often report "OK" here
         * and only emit "SEARCHING..." once the first PID is actually
         * requested and the protocol handshake happens against the ECU
         * - which is exactly when s_search_reported gets used below. */
        elm327_respond("OK\r\r>");
        return;
    }
    if (elm327_cmd_is(cmd, "ATI")) {
        elm327_respond("ELM327 v1.5\r\r>");
        return;
    }

    /* Unrecognized AT command: real ELM327 firmware replies "?" for
     * anything it doesn't understand, rather than silently ignoring it -
     * useful if DBAS's setup sequence ever grows to include a command
     * this simulator doesn't implement yet, so the mismatch is visible
     * in logs rather than DBAS just timing out. */
    ESP_LOGW(TAG, "Unrecognized AT command: \"%s\"", cmd);
    elm327_respond("?\r\r>");
}

/**
 * @brief Encode and send a Mode 01 PID response, e.g. "41 0C 1A F8\r\r>"
 *        for engine RPM. Byte encodings match the standard OBD-II Mode
 *        01 formulas (the same ones DBAS's obd.c decodes on the other
 *        end).
 */
static void elm327_handle_pid_request(uint8_t pid)
{
    pid_values_t values;
    pid_generator_get_current(&values);

    /* Emulate the one-time "SEARCHING..." protocol-detection line a
     * real adapter emits on its first PID request after power-on/reset -
     * DBAS's obd.c specifically has handling (OBD_SEARCHING_EXTRA_MS) for
     * exactly this, so reproducing it is a meaningful realism/test case,
     * not just flavor. */
    char prefix[16] = "";
    if (!s_search_reported) {
        s_search_reported = true;
        strncpy(prefix, "SEARCHING...\r", sizeof(prefix) - 1);
    }

    switch (pid) {
        case 0x0C: { /* Engine RPM: ((A*256)+B)/4 */
            uint16_t raw = (uint16_t)(values.engine_rpm * 4);
            uint8_t a = (uint8_t)(raw >> 8);
            uint8_t b = (uint8_t)(raw & 0xFF);
            elm327_respond("%s41 0C %02X %02X\r\r>", prefix, a, b);
            break;
        }
        case 0x0D: { /* Vehicle speed: A, km/h directly, 0-255 */
            uint8_t a = (uint8_t)(values.vehicle_speed_kmh > 255.0f
                                       ? 255
                                       : (values.vehicle_speed_kmh < 0.0f
                                              ? 0
                                              : values.vehicle_speed_kmh));
            elm327_respond("%s41 0D %02X\r\r>", prefix, a);
            break;
        }
        case 0x05: { /* Coolant temp: A - 40, Celsius */
            int16_t raw = (int16_t)(values.coolant_temp_c + 40);
            uint8_t a = (uint8_t)(raw < 0 ? 0 : (raw > 255 ? 255 : raw));
            elm327_respond("%s41 05 %02X\r\r>", prefix, a);
            break;
        }
        case 0x11: { /* Throttle position: A * 100 / 255, percent */
            float pct = values.throttle_position_pct;
            if (pct < 0.0f) pct = 0.0f;
            if (pct > 100.0f) pct = 100.0f;
            uint8_t a = (uint8_t)((pct * 255.0f) / 100.0f);
            elm327_respond("%s41 11 %02X\r\r>", prefix, a);
            break;
        }
        default:
            /* Unsupported PID: real ELM327 replies "NO DATA" rather than
             * silently dropping the request. */
            ESP_LOGW(TAG, "Unsupported PID requested: 0x%02X", pid);
            elm327_respond("%sNO DATA\r\r>", prefix);
            break;
    }
}

/**
 * @brief Top-level dispatch for one complete received command string.
 *        Distinguishes AT commands ("AT..." prefix) from Mode 01 PID
 *        requests (exactly 4 hex digits, e.g. "010C").
 */
static void elm327_process_command(const char *cmd)
{
    ESP_LOGI(TAG, "RX -> \"%s\"", cmd);

    /* Real adapters, while echo is enabled, transmit the command text
     * itself back before the response (this is the "echo" ATE1/ATE0
     * controls). DBAS's obd.c only ever sends ATE0 as its second setup
     * command with echo defaulting on beforehand, so this path is
     * exercised for exactly one exchange in a real bring-up sequence -
     * worth emulating faithfully rather than skipping. */
    if (s_echo_enabled) {
        char echo_buf[ELM327_CMD_MAX_LEN + 2];
        int n = snprintf(echo_buf, sizeof(echo_buf), "%s\r", cmd);
        if (n > 0) {
            elm327_send_response(echo_buf, (size_t)n);
        }
    }

    size_t cmd_len = strlen(cmd);

    if (cmd_len >= 2 && (cmd[0] == 'A' || cmd[0] == 'a') && (cmd[1] == 'T' || cmd[1] == 't')) {
        elm327_handle_at_command(cmd);
        return;
    }

    /* Mode 01 PID request: exactly 4 hex digits, "01" (mode) + 2-digit PID. */
    if (cmd_len == 4 && strncmp(cmd, "01", 2) == 0 &&
        isxdigit((unsigned char)cmd[2]) && isxdigit((unsigned char)cmd[3])) {
        char pid_str[3] = { cmd[2], cmd[3], '\0' };
        uint8_t pid = (uint8_t)strtol(pid_str, NULL, 16);
        elm327_handle_pid_request(pid);
        return;
    }

    ESP_LOGW(TAG, "Unrecognized command format: \"%s\"", cmd);
    elm327_respond("?\r\r>");
}

/* ---------------------------------------------------------------------------
 * Command Processing Task
 * ------------------------------------------------------------------------- */

static void elm327_cmd_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Command processing task started");

    elm327_cmd_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_cmd_queue, &msg, portMAX_DELAY) == pdTRUE) {
            elm327_process_command(msg.text);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Advertising
 * ------------------------------------------------------------------------- */

static void elm327_advertise_start(void)
{
    struct ble_gap_adv_params adv_params = { 0 };
    struct ble_hs_adv_fields fields = { 0 };

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    fields.name = (uint8_t *)ELM327_SIM_DEVICE_NAME;
    fields.name_len = strlen(ELM327_SIM_DEVICE_NAME);
    fields.name_is_complete = 1;

    static ble_uuid16_t svc_uuid = BLE_UUID16_INIT(ELM327_SIM_SVC_UUID);
    fields.uuids16 = &svc_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed, rc=%d", rc);
        return;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    /* Explicit fast advertising interval (units of 0.625ms: 48 = 30ms,
     * 96 = 60ms), rather than leaving these zeroed. Zero is supposed to
     * mean "let the stack pick a spec-default interval," and that's
     * almost certainly what was already happening - but making it
     * explicit removes any doubt about that when diagnosing a
     * connection-reliability issue, and a fast interval minimizes the
     * central's odds of missing an advertising event within its scan
     * window during connection establishment. */
    adv_params.itvl_min = 48;
    adv_params.itvl_max = 96;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params,
                            elm327_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed, rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "Advertising started as \"%s\" (itvl_min=%dms, itvl_max=%dms)",
             ELM327_SIM_DEVICE_NAME, adv_params.itvl_min * 625 / 1000,
             adv_params.itvl_max * 625 / 1000);
}

/* ---------------------------------------------------------------------------
 * GAP Event Handling
 * ------------------------------------------------------------------------- */

static int elm327_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) {
                ESP_LOGW(TAG, "Connection attempt failed, status=%d; resuming advertising",
                         event->connect.status);
                elm327_advertise_start();
                return 0;
            }
            ESP_LOGI(TAG, "Central connected, conn_handle=%d", event->connect.conn_handle);
            s_conn_handle = event->connect.conn_handle;
            s_att_mtu = ELM327_DEFAULT_ATT_MTU;
            s_notify_enabled = false;
            s_rx_accum_len = 0;
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGW(TAG, "Central disconnected, reason=%d; resuming advertising",
                     event->disconnect.reason);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_notify_enabled = false;
            s_rx_accum_len = 0;
            elm327_advertise_start();
            return 0;

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == s_notify_val_handle) {
                s_notify_enabled = event->subscribe.cur_notify;
                ESP_LOGI(TAG, "DBAS %s FFF1 notifications",
                         s_notify_enabled ? "subscribed to" : "unsubscribed from");
            }
            return 0;

        case BLE_GAP_EVENT_MTU:
            s_att_mtu = event->mtu.value;
            ESP_LOGI(TAG, "ATT MTU negotiated: %d", s_att_mtu);
            return 0;

        default:
            return 0;
    }
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
    /* The address override itself already happened in elm327_sim_init(),
     * BEFORE nimble_port_init() - see the comment there for why this has
     * to happen at the controller MAC level rather than here at the
     * NimBLE host level (there is no host-level "set my public address"
     * API; NimBLE expects the public address to come from the
     * controller). By the time we get here, ble_hs_id_infer_auto() will
     * simply discover the address we already forced. */
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed, rc=%d", rc);
        return;
    }

    elm327_advertise_start();
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run(); /* Returns only on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t elm327_sim_init(void)
{
    static bool initialized = false;
    if (initialized) {
        return ESP_OK;
    }

    s_cmd_queue = xQueueCreate(ELM327_CMD_QUEUE_LEN, sizeof(elm327_cmd_msg_t));
    if (s_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create command queue");
        return ESP_ERR_NO_MEM;
    }

    /* Override the controller's Bluetooth MAC address to match DBAS's
     * hardcoded OBD_BT_TARGET_MAC BEFORE the BT controller/NimBLE stack
     * starts - see the integration note at the top of this file for why
     * this has to happen (DBAS connects by fixed address, never by
     * scan). This must happen before nimble_port_init(), since that is
     * what brings up the controller and has it read its MAC; setting
     * this afterward would be too late. There is no equivalent NimBLE
     * *host*-level API for this (ble_hs_id_set_pub() does not exist -
     * NimBLE expects its public address to already be correct by the
     * time the host syncs with the controller), which is why this lives
     * here rather than in ble_on_sync(). */
    esp_err_t mac_err = esp_iface_mac_addr_set(ELM327_SIM_TARGET_MAC, ESP_MAC_BT);
    if (mac_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_iface_mac_addr_set(ESP_MAC_BT) failed: %s - advertising with "
                 "factory address instead; DBAS's hardcoded-MAC connect will NOT find "
                 "this device until this is resolved", esp_err_to_name(mac_err));
    } else {
        ESP_LOGI(TAG, "Bluetooth MAC overridden to match DBAS's OBD_BT_TARGET_MAC");
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed, rc=%d", rc);
        return ESP_ERR_INVALID_STATE;
    }

    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed, rc=%d", rc);
        return ESP_ERR_INVALID_STATE;
    }

    rc = ble_svc_gap_device_name_set(ELM327_SIM_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_svc_gap_device_name_set failed, rc=%d", rc);
    }

    BaseType_t task_created = xTaskCreate(
        elm327_cmd_task, "elm327_cmd_task", 4096, NULL, 5, NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create command processing task");
        return ESP_ERR_NO_MEM;
    }

    nimble_port_freertos_init(ble_host_task);

    initialized = true;
    ESP_LOGI(TAG, "ELM327 BLE simulator initialized");
    return ESP_OK;
}
