/**
 * @file ble_hid_gatt.c
 * @brief HOGP keyboard via Espressif esp_hid (NimBLE ble_svc_hid).
 *
 * Full HID-over-GATT: DIS, Battery, Report Map with Input + LED Output,
 * Boot Keyboard Input/Output. Matches the IDF esp_hid_device keyboard profile
 * so phones/OS hosts can subscribe and type reliably.
 */

#include "ble_hid_gatt.h"
#include "ble_hid.h"

#include "esp_err.h"
#include "esp_hid_common.h"
#include "esp_hidd.h"
#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "services/gap/ble_svc_gap.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "ble_hid_gatt";

/* Boot-compatible 8-byte keyboard input + 1-byte LED output (HOGP / USB HID). */
static const uint8_t hid_report_map[] = {
    0x05, 0x01, /* Usage Page (Generic Desktop) */
    0x09, 0x06, /* Usage (Keyboard) */
    0xA1, 0x01, /* Collection (Application) */
    0x85, 0x01, /*   Report ID (1) */
    /* Modifiers */
    0x05, 0x07,
    0x19, 0xE0,
    0x29, 0xE7,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x08,
    0x81, 0x02,
    /* Reserved */
    0x95, 0x01,
    0x75, 0x08,
    0x81, 0x03,
    /* LEDs (Num/Caps/Scroll/Compose/Kana) — required by many hosts */
    0x95, 0x05,
    0x75, 0x01,
    0x05, 0x08,
    0x19, 0x01,
    0x29, 0x05,
    0x91, 0x02,
    0x95, 0x01,
    0x75, 0x03,
    0x91, 0x03,
    /* Six keycodes (standard boot keyboard length) */
    0x95, 0x06,
    0x75, 0x08,
    0x15, 0x00,
    0x25, 0x65,
    0x05, 0x07,
    0x19, 0x00,
    0x29, 0x65,
    0x81, 0x00,
    0xC0,
};

static esp_hidd_dev_t *g_hid_dev = NULL;
static int g_conn_handle = -1;
static bool g_notify_enabled = false;
static bool g_encrypted = false;

static void ble_hidd_event_cb(void *handler_args, esp_event_base_t base,
                             int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "esp_hid HOGP stack started");
        break;
    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "esp_hid CONNECT");
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        ESP_LOGI(TAG, "esp_hid DISCONNECT reason=%d",
                 param ? param->disconnect.reason : -1);
        g_conn_handle = -1;
        g_notify_enabled = false;
        g_encrypted = false;
        break;
    case ESP_HIDD_OUTPUT_EVENT:
        /* Host LED report (Caps Lock etc.) — ignore content for now. */
        ESP_LOGD(TAG, "LED output report id=%u len=%u",
                 param ? param->output.report_id : 0,
                 param ? param->output.length : 0);
        break;
    case ESP_HIDD_PROTOCOL_MODE_EVENT:
        ESP_LOGI(TAG, "Protocol mode -> %u",
                 param ? param->protocol_mode.protocol_mode : 0);
        break;
    default:
        break;
    }
}

esp_err_t ble_hid_gatt_init(void)
{
    if (g_hid_dev) {
        return ESP_OK;
    }

    /* Just Works bonding — Buddy has no real display/keyboard. DisplayOnly
     * caused phones to show random numeric-comparison codes instead of a PIN. */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    static esp_hid_raw_report_map_t report_maps[] = {
        {
            .data = hid_report_map,
            .len = sizeof(hid_report_map),
        },
    };
    static esp_hid_device_config_t config = {
        .vendor_id = 0x16C0,
        .product_id = 0x05DF,
        .version = 0x0100,
        .device_name = "Neo2 Buddy",
        .manufacturer_name = "Neo2 Buddy",
        .serial_number = "1",
        .report_maps = report_maps,
        .report_maps_len = 1,
    };

    esp_err_t err = esp_hidd_dev_init(&config, ESP_HID_TRANSPORT_BLE,
                                      ble_hidd_event_cb, &g_hid_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidd_dev_init failed: %s", esp_err_to_name(err));
        g_hid_dev = NULL;
        return err;
    }

    (void)ble_svc_gap_device_appearance_set(ESP_HID_APPEARANCE_KEYBOARD);
    ESP_LOGI(TAG, "esp_hid HOGP keyboard ready (report id 1, 8-byte + LED out)");
    return ESP_OK;
}

esp_err_t ble_hid_gatt_send_report(const uint8_t *report, size_t len)
{
    if (!g_hid_dev || !esp_hidd_dev_connected(g_hid_dev)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!report || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* esp_hidd wants a mutable buffer. */
    uint8_t buf[8];
    if (len > sizeof(buf)) {
        len = sizeof(buf);
    }
    memcpy(buf, report, len);
    if (len < sizeof(buf)) {
        memset(buf + len, 0, sizeof(buf) - len);
        len = sizeof(buf);
    }

    esp_err_t err = esp_hidd_dev_input_set(g_hid_dev, 0, 1, buf, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_hidd_dev_input_set failed: %s", esp_err_to_name(err));
    }
    return err;
}

bool ble_hid_gatt_is_connected(void)
{
    return g_hid_dev != NULL && esp_hidd_dev_connected(g_hid_dev);
}

void ble_hid_gatt_disconnect(void)
{
    if (g_conn_handle >= 0) {
        (void)ble_gap_terminate(g_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    /* Fallback: terminate first active connection if any. */
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(0, &desc) == 0) {
        (void)ble_gap_terminate(desc.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

bool ble_hid_gatt_can_send(void)
{
    if (!ble_hid_gatt_is_connected()) {
        return false;
    }
    /* Prefer CCCD-ready; fall back to encrypted link (hosts often enable notify
     * in the same window as bonding). */
    return g_notify_enabled || g_encrypted;
}

void ble_hid_gatt_gap_event(struct ble_gap_event *event)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            g_conn_handle = event->connect.conn_handle;
            g_notify_enabled = false;
            g_encrypted = false;
            ESP_LOGI(TAG, "HID link up conn_handle=%d", g_conn_handle);
            if (g_hid_dev) {
                (void)esp_hidd_dev_battery_set(g_hid_dev, 100);
            }
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        g_conn_handle = -1;
        g_notify_enabled = false;
        g_encrypted = false;
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.cur_notify) {
            g_notify_enabled = true;
            ESP_LOGI(TAG, "Host enabled notifications (handle=%u) — typing ready",
                     event->subscribe.attr_handle);
        } else if (!event->subscribe.cur_notify && !event->subscribe.cur_indicate) {
            ESP_LOGD(TAG, "Unsubscribe handle=%u", event->subscribe.attr_handle);
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        g_encrypted = (event->enc_change.status == 0);
        if (g_encrypted) {
            ESP_LOGI(TAG, "Encrypted — can_send=%d notify=%d",
                     ble_hid_gatt_can_send() ? 1 : 0,
                     g_notify_enabled ? 1 : 0);
        }
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        /* Just Works should not need this; accept anything hosts still request. */
        struct ble_sm_io pkey = {0};
        int rc = 0;
        if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            pkey.action = event->passkey.params.action;
            pkey.numcmp_accept = 1;
            ESP_LOGI(TAG, "Numeric comparison %06" PRIu32 " — accepting (tap Pair on phone)",
                     (uint32_t)event->passkey.params.numcmp);
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        } else if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            pkey.action = event->passkey.params.action;
            pkey.passkey = BLE_HID_PAIRING_PASSKEY;
            ESP_LOGW(TAG, "Unexpected passkey display request — using %06" PRIu32,
                     pkey.passkey);
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        } else if (event->passkey.params.action == BLE_SM_IOACT_INPUT) {
            pkey.action = event->passkey.params.action;
            pkey.passkey = BLE_HID_PAIRING_PASSKEY;
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        } else if (event->passkey.params.action == BLE_SM_IOACT_OOB) {
            pkey.action = event->passkey.params.action;
            memset(pkey.oob, 0, sizeof(pkey.oob));
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        }
        if (rc != 0) {
            ESP_LOGW(TAG, "ble_sm_inject_io: %d", rc);
        }
        break;
    }
    default:
        break;
    }
}
