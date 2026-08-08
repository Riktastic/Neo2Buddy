/**
 * @file ble_hid_gatt.c
 * @brief GATT HID keyboard service for NimBLE.
 */

#include "ble_hid_gatt.h"
#include "esp_err.h"
#include "esp_log.h"
#include <stdbool.h>
#include <string.h>

#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_hid_gatt";

static const uint8_t hid_report_map[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x01,
    0x75, 0x08, 0x95, 0x06, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0
};

static int g_conn_handle = -1;
static uint16_t g_input_report_val_handle;
static bool g_input_notify_enabled = false;
static uint8_t g_protocol_mode = 1;
static uint8_t g_last_report[8];

static int hid_info_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg);
static int report_map_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg);
static int hid_control_point_access(uint16_t conn_handle, uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt *ctxt, void *arg);
static int protocol_mode_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg);
static int report_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg);
static int report_ref_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(0x2A4A),
                .access_cb = hid_info_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = BLE_UUID16_DECLARE(0x2A4B),
                .access_cb = report_map_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = BLE_UUID16_DECLARE(0x2A4C),
                .access_cb = hid_control_point_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = BLE_UUID16_DECLARE(0x2A4E),
                .access_cb = protocol_mode_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = BLE_UUID16_DECLARE(0x2A4D),
                .access_cb = report_access,
                .val_handle = &g_input_report_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    {
                        .uuid = BLE_UUID16_DECLARE(0x2908),
                        .access_cb = report_ref_access,
                        .att_flags = BLE_ATT_F_READ,
                    },
                    { 0 },
                },
            },
            { 0 },
        },
    },
    { 0 },
};

esp_err_t ble_hid_gatt_init(void)
{
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "HID GATT initialized (report handle=%u)", g_input_report_val_handle);
    return ESP_OK;
}

esp_err_t ble_hid_gatt_send_report(const uint8_t *report, size_t len)
{
    if (g_conn_handle < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!g_input_report_val_handle) {
        return ESP_FAIL;
    }
    if (len > sizeof(g_last_report)) {
        len = sizeof(g_last_report);
    }
    memcpy(g_last_report, report, len);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, len);
    if (!om) {
        return ESP_ERR_NO_MEM;
    }

    int rc = ble_gatts_notify_custom(g_conn_handle, g_input_report_val_handle, om);
    if (rc != 0) {
        os_mbuf_free_chain(om);
        ESP_LOGE(TAG, "ble_gatts_notify_custom failed: %d", rc);
        return ESP_FAIL;
    }

    return ESP_OK;
}

bool ble_hid_gatt_is_connected(void)
{
    return g_conn_handle >= 0;
}

bool ble_hid_gatt_can_send(void)
{
    return g_conn_handle >= 0 && g_input_report_val_handle != 0 && g_input_notify_enabled;
}

void ble_hid_gatt_gap_event(struct ble_gap_event *event)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            g_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "HID GATT connected conn_handle=%d", g_conn_handle);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        g_conn_handle = -1;
        g_input_notify_enabled = false;
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == g_input_report_val_handle) {
            g_input_notify_enabled = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "Input report notifications %s",
                     g_input_notify_enabled ? "enabled" : "disabled");
        }
        break;
    default:
        break;
    }
}

static int hid_info_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const uint8_t hid_info[] = { 0x11, 0x01, 0x00, 0x03 };
        return os_mbuf_append(ctxt->om, hid_info, sizeof(hid_info)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int report_map_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, hid_report_map, sizeof(hid_report_map)) == 0
                   ? 0
                   : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int hid_control_point_access(uint16_t conn_handle, uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int protocol_mode_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, &g_protocol_mode, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t mode = 0;
        if (OS_MBUF_PKTLEN(ctxt->om) != 1) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        os_mbuf_copydata(ctxt->om, 0, 1, &mode);
        g_protocol_mode = mode;
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int report_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, g_last_report, sizeof(g_last_report)) == 0
                   ? 0
                   : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int report_ref_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    static const uint8_t ref[2] = { 0x00, 0x01 };
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
        return os_mbuf_append(ctxt->om, ref, sizeof(ref)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}
