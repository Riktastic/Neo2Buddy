/**
 * @file ble_hid.c
 * @brief NimBLE HID keyboard: Neo passthrough, pairing, persistent bonds, portal send.
 */

#include "ble_hid.h"
#include "settings.h"
#include "neo_live.h"
#include "device_status.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ble_hid_gatt.h"
#include "ble_hid_device.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>

void ble_store_config_init(void);

static const char *TAG = "ble_hid";

#define BLE_PREVIEW_MAX 4096
#define BLE_PAIRING_TIMEOUT_US (120 * 1000000LL)
#define BLE_PASS_QUEUE_LEN 24

typedef struct {
    uint8_t report[8];
} ble_pass_item_t;

static bool g_advertising = false;
static bool g_pairing_enabled = false;
static esp_timer_handle_t g_pairing_timer = NULL;

static char g_preview_text[BLE_PREVIEW_MAX];
static size_t g_preview_len = 0;
static bool g_preview_ready = false;

static volatile bool g_send_cancel = false;
static volatile bool g_send_running = false;
static TaskHandle_t g_send_task = NULL;

static QueueHandle_t g_pass_q = NULL;
static TaskHandle_t g_pass_task = NULL;
static bool g_ble_ready = false;

static void ble_app_on_sync(void);
static int ble_gap_event(struct ble_gap_event *event, void *arg);
static void ble_host_task(void *param);
static void pairing_timeout_cb(void *arg);
static void update_ble_status(void);
static void send_task(void *arg);
static void pass_task(void *arg);
static void ble_hid_maybe_advertise(void);

static int bonded_peer_count(void)
{
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int count = CONFIG_BT_NIMBLE_MAX_BONDS;
    int rc = ble_store_util_bonded_peers(peers, &count, CONFIG_BT_NIMBLE_MAX_BONDS);
    if (rc != 0 || count < 0) {
        return 0;
    }
    return count;
}

int ble_hid_bonded_count(void)
{
    return bonded_peer_count();
}

void ble_hid_clear_bonds(void)
{
    if (!g_ble_ready) {
        return;
    }
    ble_store_clear();
    ESP_LOGI(TAG, "Cleared BLE bond store");
}

static void pairing_timeout_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Pairing window expired");
    ble_hid_set_pairing_enabled(false);
}

static void update_ble_status(void)
{
    if (ble_hid_gatt_is_connected()) {
        device_status_set_ble(DEVICE_BLE_CONNECTED);
    } else if (g_pairing_enabled && g_advertising) {
        device_status_set_ble(DEVICE_BLE_PAIRING);
    } else {
        device_status_set_ble(DEVICE_BLE_IDLE);
    }
}

static void pass_task(void *arg)
{
    (void)arg;
    ble_pass_item_t item;
    for (;;) {
        if (xQueueReceive(g_pass_q, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (g_send_running) {
            continue;
        }
        if (!ble_hid_gatt_can_send()) {
            continue;
        }
        ble_hid_device_send_report(item.report, sizeof(item.report));
    }
}

void ble_hid_passthrough_report(const uint8_t *report, size_t len)
{
    if (!report || len < 8 || g_pass_q == NULL) {
        return;
    }
    if (g_send_running) {
        return;
    }
    if (!ble_hid_gatt_can_send()) {
        return;
    }
    ble_pass_item_t item;
    memcpy(item.report, report, 8);
    (void)xQueueSend(g_pass_q, &item, 0);
}

void ble_hid_init(void)
{
    ble_store_config_init();

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return;
    }

    ble_hs_cfg.sync_cb = ble_app_on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.reset_cb = NULL;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    if (g_pass_q == NULL) {
        g_pass_q = xQueueCreate(BLE_PASS_QUEUE_LEN, sizeof(ble_pass_item_t));
    }
    if (g_pass_q != NULL && g_pass_task == NULL) {
        if (xTaskCreate(pass_task, "ble_pass", 3072, NULL, 5, &g_pass_task) != pdPASS) {
            ESP_LOGW(TAG, "BLE passthrough task not started");
            g_pass_task = NULL;
        }
    }

    nimble_port_freertos_init(ble_host_task);
    g_ble_ready = true;
    device_status_set_ble(DEVICE_BLE_IDLE);
    ESP_LOGI(TAG, "NimBLE init complete (passthrough + NVS bonds)");
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_hid_deinit(void)
{
    ble_hid_cancel_send();
    ble_hid_set_pairing_enabled(false);
    nimble_port_deinit();
}

static void ble_app_on_sync(void)
{
    ble_hs_util_ensure_addr(0);

    const char *name = settings_get_device_name();
    if (!name || name[0] == '\0') {
        name = "Neo2 Buddy";
    }
    ble_svc_gap_device_name_set(name);
    if (ble_hid_gatt_init() != ESP_OK) {
        ESP_LOGE(TAG, "HID GATT setup failed — Bluetooth keyboard unavailable");
        return;
    }

    int bonds = bonded_peer_count();
    ESP_LOGI(TAG, "BLE sync, device name: %s, bonded hosts: %d", name, bonds);
    ble_hid_maybe_advertise();
}

void ble_hid_set_pairing_enabled(bool enabled)
{
    g_pairing_enabled = enabled;
    if (enabled) {
        /* Restart advertising without whitelist so a new host can bond. */
        ble_hid_stop_advertising();
        ble_hid_start_advertising(true);
        if (g_pairing_timer) {
            esp_timer_stop(g_pairing_timer);
        } else {
            const esp_timer_create_args_t args = {
                .callback = pairing_timeout_cb,
                .name = "ble_pair",
            };
            esp_timer_create(&args, &g_pairing_timer);
        }
        esp_timer_start_once(g_pairing_timer, BLE_PAIRING_TIMEOUT_US);
    } else {
        if (g_pairing_timer) {
            esp_timer_stop(g_pairing_timer);
        }
        /* Drop open advertising, then re-open for bonded reconnect if needed. */
        ble_hid_stop_advertising();
        ble_hid_maybe_advertise();
    }
    update_ble_status();
}

bool ble_hid_pairing_enabled(void)
{
    return g_pairing_enabled;
}

static void ble_hid_maybe_advertise(void)
{
    if (ble_hid_gatt_is_connected()) {
        return;
    }
    if (g_pairing_enabled || bonded_peer_count() > 0) {
        ble_hid_start_advertising(g_pairing_enabled);
    }
}

void ble_hid_start_advertising(bool allow_pairing)
{
    if (g_advertising) {
        /* Already advertising — restart if filter policy must change. */
        ble_hid_stop_advertising();
    }

    struct ble_hs_adv_fields fields = {0};
    const char *name = settings_get_device_name();
    if (!name || name[0] == '\0') {
        name = "Neo2 Buddy";
    }

    static ble_uuid16_t adv_uuids16[] = {
        BLE_UUID16_INIT(0x1812),
    };

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    fields.appearance = 0x03C1;
    fields.appearance_is_present = 1;
    fields.uuids16 = adv_uuids16;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set advertising fields: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.filter_policy = 0;

    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int peer_count = CONFIG_BT_NIMBLE_MAX_BONDS;
    if (!allow_pairing &&
        ble_store_util_bonded_peers(peers, &peer_count, CONFIG_BT_NIMBLE_MAX_BONDS) == 0 &&
        peer_count > 0) {
        rc = ble_gap_wl_set(peers, peer_count);
        if (rc == 0) {
            /* Scan from all, connect only from bonded whitelist. */
            adv_params.filter_policy = BLE_HCI_ADV_FILT_CONN;
            ESP_LOGI(TAG, "Reconnect advertising for %d bonded host(s)", peer_count);
        } else {
            ESP_LOGW(TAG, "Whitelist set failed (%d); open advertising for reconnect", rc);
        }
    } else if (allow_pairing) {
        ESP_LOGI(TAG, "Pairing advertising (open)");
    }

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event, NULL);
    if (rc == 0) {
        g_advertising = true;
        ESP_LOGI(TAG, "Started BLE advertising");
    } else {
        ESP_LOGE(TAG, "Failed to start advertising: %d", rc);
    }
    update_ble_status();
}

void ble_hid_stop_advertising(void)
{
    if (!g_advertising) {
        return;
    }
    ble_gap_adv_stop();
    g_advertising = false;
    update_ble_status();
}

bool ble_hid_is_advertising(void)
{
    return g_advertising;
}

bool ble_hid_is_connected(void)
{
    return ble_hid_gatt_is_connected();
}

bool ble_hid_can_send(void)
{
    return ble_hid_gatt_can_send();
}

bool ble_hid_send_in_progress(void)
{
    return g_send_running;
}

esp_err_t ble_hid_preview_text(const char *text, char *preview_out, size_t preview_size, size_t *total_len)
{
    if (!text || !preview_out || preview_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(text);
    if (len == 0 || len >= BLE_PREVIEW_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (g_send_running) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(g_preview_text, text, len + 1);
    g_preview_len = len;
    g_preview_ready = true;

    size_t copy = len;
    if (copy >= preview_size) {
        copy = preview_size - 1;
    }
    memcpy(preview_out, text, copy);
    preview_out[copy] = '\0';
    if (total_len) {
        *total_len = len;
    }
    return ESP_OK;
}

static void send_task(void *arg)
{
    (void)arg;
    g_send_cancel = false;
    g_send_running = true;

    const char *text = g_preview_text;
    size_t len = g_preview_len;
    g_preview_ready = false;

    for (size_t i = 0; i < len && !g_send_cancel; ++i) {
        if (!ble_hid_gatt_can_send()) {
            ESP_LOGW(TAG, "BLE host disconnected during send");
            break;
        }
        char chunk[2] = { text[i], '\0' };
        ble_hid_device_send_char(text[i]);
        neo_live_append(chunk, 1);
        vTaskDelay(pdMS_TO_TICKS(12));
    }

    g_send_running = false;
    g_send_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t ble_hid_confirm_send(void)
{
    if (!g_preview_ready || g_preview_len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!ble_hid_gatt_can_send()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (g_send_running) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xTaskCreate(send_task, "ble_send", 4096, NULL, 5, &g_send_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ble_hid_cancel_send(void)
{
    g_send_cancel = true;
    g_preview_ready = false;
    g_preview_len = 0;
    g_preview_text[0] = '\0';
}

esp_err_t ble_hid_send_text(const char *text)
{
    if (!text || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ble_hid_gatt_can_send()) {
        return ESP_ERR_INVALID_STATE;
    }
    char preview[256];
    size_t total = 0;
    esp_err_t err = ble_hid_preview_text(text, preview, sizeof(preview), &total);
    if (err != ESP_OK) {
        return err;
    }
    return ble_hid_confirm_send();
}

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "BLE host connected");
            ble_hid_stop_advertising();
            if (g_pairing_timer) {
                esp_timer_stop(g_pairing_timer);
            }
            g_pairing_enabled = false;
        } else {
            ESP_LOGI(TAG, "BLE connection failed; status=%d", event->connect.status);
            ble_hid_maybe_advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE host disconnected; reason=%d", event->disconnect.reason);
        ble_hid_cancel_send();
        ble_hid_maybe_advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        g_advertising = false;
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "BLE encryption %s",
                 event->enc_change.status == 0 ? "established" : "failed");
        break;
    default:
        break;
    }

    ble_hid_gatt_gap_event(event);
    update_ble_status();
    return 0;
}
