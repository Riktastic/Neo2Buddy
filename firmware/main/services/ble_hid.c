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
#include "nvs.h"
#include "ble_hid_gatt.h"
#include "ble_hid_device.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/ble.h"
#include "services/gap/ble_svc_gap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include <string.h>

void ble_store_config_init(void);

static const char *TAG = "ble_hid";

#define BLE_PREVIEW_MAX 4096
#define BLE_PAIRING_TIMEOUT_US (180 * 1000000LL)
#define BLE_PASS_QUEUE_LEN 24
#define BLE_HID_NVS_NS "ble_hid"
#define BLE_HID_NVS_BONDS "bonds"
#define BLE_HID_NVS_PEERS "peers"
typedef struct {
    uint8_t report[8];
} ble_pass_item_t;

static bool g_advertising = false;
static bool g_pairing_enabled = false;
static esp_timer_handle_t g_pairing_timer = NULL;
/* Held from early boot until nimble_port_init so Wi‑Fi/httpd cannot fragment
 * the contiguous INTERNAL block the BT controller requires. */
static void *g_ble_ram_hold = NULL;
static size_t g_ble_ram_hold_size = 0;
static volatile bool g_ble_starting = false;

/* Prefer PSRAM so preview text does not permanently occupy INTERNAL DRAM. */
static char *g_preview_text = NULL;
static size_t g_preview_len = 0;
static bool g_preview_ready = false;

static volatile bool g_send_cancel = false;
static volatile bool g_send_running = false;
static TaskHandle_t g_send_task = NULL;

static QueueHandle_t g_pass_q = NULL;
static TaskHandle_t g_pass_task = NULL;
static bool g_ble_ready = false;
static bool g_synced = false;

/* If a host connects during pairing but never bonds, free the single slot. */
#define BLE_PAIR_ENC_DEADLINE_US (45 * 1000000LL)
/* Let Windows/Android finish ATT discovery before we push pairing. */
#define BLE_PAIR_SEC_DELAY_US (150 * 1000LL)

static ble_addr_t g_active_peer;
static bool g_active_peer_valid = false;
static bool g_active_peer_encrypted = false;
static esp_timer_handle_t g_pair_enc_timer = NULL;
static esp_timer_handle_t g_pair_sec_timer = NULL;
static uint16_t g_pair_sec_conn = 0;

static void ble_app_on_sync(void);
static int ble_gap_event(struct ble_gap_event *event, void *arg);
static void ble_host_task(void *param);
static void pairing_timeout_cb(void *arg);
static void pair_enc_deadline_cb(void *arg);
static void pair_sec_delay_cb(void *arg);
static void update_ble_status(void);
static void send_task(void *arg);
static void pass_task(void *arg);
static void ble_hid_maybe_advertise(void);
static uint8_t ble_hid_load_bond_hint(void);
static void ble_hid_refresh_bond_hint(void);
static void ble_hid_persist_peers(const ble_addr_t *peers, int count);
static void ble_hid_clear_stale_legacy_nvs(void);
static void ble_pair_enc_timer_stop(void);
static void ble_pair_sec_timer_stop(void);

static void ble_pair_enc_timer_stop(void)
{
    if (g_pair_enc_timer) {
        esp_timer_stop(g_pair_enc_timer);
    }
}

static void ble_pair_sec_timer_stop(void)
{
    if (g_pair_sec_timer) {
        esp_timer_stop(g_pair_sec_timer);
    }
    g_pair_sec_conn = 0;
}

static void pair_sec_delay_cb(void *arg)
{
    (void)arg;
    uint16_t conn = g_pair_sec_conn;
    g_pair_sec_conn = 0;
    if (conn == 0) {
        return;
    }
    int sec = ble_gap_security_initiate(conn);
    if (sec != 0 && sec != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_security_initiate (delayed): %d", sec);
    } else {
        ESP_LOGI(TAG, "Security/bonding requested on conn=%u", (unsigned)conn);
    }
}

static void pair_enc_deadline_cb(void *arg)
{
    (void)arg;
    if (!g_pairing_enabled || g_active_peer_encrypted || !g_active_peer_valid) {
        return;
    }
    if (!ble_hid_gatt_is_connected()) {
        return;
    }
    ESP_LOGW(TAG, "Pairing: no encryption within deadline — dropping link for next host");
    ble_hid_gatt_disconnect();
}

/** Ask the host for a short connection interval (keystroke latency). */
static void ble_request_low_latency_params(uint16_t conn_handle)
{
    struct ble_gap_upd_params up = {
        .itvl_min = 6, /* 7.5 ms */
        .itvl_max = 9, /* 11.25 ms */
        .latency = 0,
        .supervision_timeout = 400,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    int urc = ble_gap_update_params(conn_handle, &up);
    if (urc != 0 && urc != BLE_HS_EALREADY) {
        ESP_LOGD(TAG, "ble_gap_update_params: %d", urc);
    }
}

static bool ble_hid_ensure_preview_buf(void)
{
    if (g_preview_text) {
        return true;
    }
    g_preview_text = heap_caps_malloc(BLE_PREVIEW_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_preview_text) {
        g_preview_text = heap_caps_malloc(BLE_PREVIEW_MAX, MALLOC_CAP_8BIT);
    }
    return g_preview_text != NULL;
}

static void ble_hid_release_controller_ram(void)
{
    if (g_ble_ram_hold) {
        heap_caps_free(g_ble_ram_hold);
        ESP_LOGI(TAG, "Released BLE INTERNAL hold (%u bytes)",
                 (unsigned)g_ble_ram_hold_size);
        g_ble_ram_hold = NULL;
        g_ble_ram_hold_size = 0;
    }
}

void ble_hid_hold_controller_ram(void)
{
    if (g_ble_ready || g_ble_ram_hold) {
        return;
    }
    /* Prefer DMA-capable INTERNAL — the controller EMI path needs it. */
    const uint32_t caps_dma = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    const uint32_t caps_any = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    /* Try preferred → smaller so we still pin something useful on tight boots. */
    static const size_t sizes[] = {
        BLE_HID_MIN_INTERNAL_HEAP, /* 28 KB preferred */
        BLE_HID_CONTROLLER_FLOOR,  /* 24 KB absolute floor */
    };

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        const size_t want = sizes[i];
        g_ble_ram_hold = heap_caps_malloc(want, caps_dma);
        if (!g_ble_ram_hold) {
            g_ble_ram_hold = heap_caps_malloc(want, caps_any);
        }
        if (g_ble_ram_hold) {
            g_ble_ram_hold_size = want;
            memset(g_ble_ram_hold, 0, g_ble_ram_hold_size);
            ESP_LOGI(TAG,
                     "Pinned BLE INTERNAL hold (%u bytes%s); largest_internal now=%u",
                     (unsigned)g_ble_ram_hold_size,
                     (want < BLE_HID_MIN_INTERNAL_HEAP) ? ", reduced" : "",
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            return;
        }
    }

    ESP_LOGW(TAG,
             "Could not pin BLE INTERNAL hold (tried %u..%u); pairing may fail after Wi‑Fi starts "
             "(largest_internal=%u)",
             (unsigned)BLE_HID_MIN_INTERNAL_HEAP, (unsigned)BLE_HID_CONTROLLER_FLOOR,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
}

bool ble_hid_radio_critical(void)
{
    if (g_ble_starting || g_pairing_enabled) {
        return true;
    }
    if (g_ble_ready && ble_hid_gatt_is_connected()) {
        return true;
    }
    return false;
}

static bool ble_hid_have_controller_ram(void)
{
    if (g_ble_ram_hold && g_ble_ram_hold_size >= BLE_HID_CONTROLLER_FLOOR) {
        return true;
    }
    size_t largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (largest < BLE_HID_CONTROLLER_FLOOR) {
        size_t free_internal =
            heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        size_t free_spiram =
            heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
        ESP_LOGW(TAG,
                 "BLE deferred: need largest_internal>=%u (have %u); "
                 "free_internal=%u free_spiram=%u (controller needs INTERNAL, not PSRAM)",
                 (unsigned)BLE_HID_CONTROLLER_FLOOR, (unsigned)largest,
                 (unsigned)free_internal, (unsigned)free_spiram);
        return false;
    }
    return true;
}

bool ble_hid_is_ready(void)
{
    return g_ble_ready;
}

static uint8_t ble_hid_load_bond_hint(void)
{
    nvs_handle_t h;
    uint8_t count = 0;
    if (nvs_open(BLE_HID_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    (void)nvs_get_u8(h, BLE_HID_NVS_BONDS, &count);
    nvs_close(h);
    return count;
}

/** Packed snapshot: type + 6-byte addr (NimBLE order) per peer. */
static void ble_hid_persist_peers(const ble_addr_t *peers, int count)
{
    nvs_handle_t h;
    if (nvs_open(BLE_HID_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (count <= 0 || !peers) {
        (void)nvs_erase_key(h, BLE_HID_NVS_PEERS);
        (void)nvs_set_u8(h, BLE_HID_NVS_BONDS, 0);
    } else {
        uint8_t blob[CONFIG_BT_NIMBLE_MAX_BONDS * 7];
        if (count > CONFIG_BT_NIMBLE_MAX_BONDS) {
            count = CONFIG_BT_NIMBLE_MAX_BONDS;
        }
        for (int i = 0; i < count; ++i) {
            blob[i * 7] = peers[i].type;
            memcpy(&blob[i * 7 + 1], peers[i].val, 6);
        }
        (void)nvs_set_blob(h, BLE_HID_NVS_PEERS, blob, (size_t)count * 7);
        (void)nvs_set_u8(h, BLE_HID_NVS_BONDS, (uint8_t)count);
    }
    (void)nvs_commit(h);
    nvs_close(h);
}

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

static void ble_hid_refresh_bond_hint(void)
{
    if (!g_synced) {
        return;
    }
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int count = CONFIG_BT_NIMBLE_MAX_BONDS;
    if (ble_store_util_bonded_peers(peers, &count, CONFIG_BT_NIMBLE_MAX_BONDS) != 0 || count < 0) {
        count = 0;
    }
    ble_hid_persist_peers(peers, count);
}

int ble_hid_bonded_count(void)
{
    if (g_synced) {
        return bonded_peer_count();
    }
    return (int)ble_hid_load_bond_hint();
}

int ble_hid_list_bonds(ble_hid_bond_peer_t *out, int max)
{
    if (!out || max <= 0) {
        return 0;
    }
    if (max > CONFIG_BT_NIMBLE_MAX_BONDS) {
        max = CONFIG_BT_NIMBLE_MAX_BONDS;
    }

    if (g_synced) {
        ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
        int count = CONFIG_BT_NIMBLE_MAX_BONDS;
        if (ble_store_util_bonded_peers(peers, &count, CONFIG_BT_NIMBLE_MAX_BONDS) != 0 || count < 0) {
            return 0;
        }
        if (count > max) {
            count = max;
        }
        for (int i = 0; i < count; ++i) {
            out[i].type = peers[i].type;
            /* Present MSB-first for humans (opposite of NimBLE wire order). */
            for (int b = 0; b < 6; ++b) {
                out[i].addr[b] = peers[i].val[5 - b];
            }
        }
        ble_hid_persist_peers(peers, count);
        return count;
    }

    nvs_handle_t h;
    if (nvs_open(BLE_HID_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    uint8_t blob[CONFIG_BT_NIMBLE_MAX_BONDS * 7];
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(h, BLE_HID_NVS_PEERS, blob, &len);
    nvs_close(h);
    if (err != ESP_OK || len < 7) {
        return 0;
    }
    int count = (int)(len / 7);
    if (count > max) {
        count = max;
    }
    for (int i = 0; i < count; ++i) {
        out[i].type = blob[i * 7];
        const uint8_t *v = &blob[i * 7 + 1];
        for (int b = 0; b < 6; ++b) {
            out[i].addr[b] = v[5 - b];
        }
    }
    return count;
}

void ble_hid_clear_bonds(void)
{
    /* Close pairing window first so we do not re-advertise for reconnect. */
    g_pairing_enabled = false;
    if (g_pairing_timer) {
        esp_timer_stop(g_pairing_timer);
    }
    ble_pair_enc_timer_stop();
    ble_pair_sec_timer_stop();
    g_active_peer_valid = false;
    g_active_peer_encrypted = false;

    ble_hid_cancel_send();
    ble_hid_gatt_disconnect();
    ble_hid_stop_advertising();

    if (g_ble_ready) {
        ble_store_clear();
    }
    /* Also wipe legacy NimBLE NVS namespaces left by older builds. */
    ble_hid_clear_stale_legacy_nvs();
    ble_hid_persist_peers(NULL, 0);

    ESP_LOGI(TAG, "Cleared all BLE bonded hosts");
    update_ble_status();
}

static void pairing_timeout_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Pairing window expired");
    (void)ble_hid_set_pairing_enabled(false);
}

static void update_ble_status(void)
{
    if (ble_hid_gatt_is_connected()) {
        device_status_set_ble(DEVICE_BLE_CONNECTED);
    } else if (g_pairing_enabled) {
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
    if (!report || len < 8) {
        return;
    }
    if (g_send_running) {
        return;
    }
    if (!ble_hid_gatt_can_send()) {
        static int64_t last_warn_us;
        int64_t now = esp_timer_get_time();
        if (now - last_warn_us > 2000000) {
            last_warn_us = now;
            ESP_LOGW(TAG,
                     "Neo key ignored — BLE connected but phone has not enabled HID notifications yet "
                     "(open a text field on the phone; forget+re-pair if this persists)");
        }
        return;
    }
    /* Fast path: notify immediately (USB host task → HOGP). Queue only if busy. */
    if (ble_hid_gatt_send_report(report, 8) == ESP_OK) {
        return;
    }
    if (g_pass_q == NULL) {
        return;
    }
    ble_pass_item_t item;
    memcpy(item.report, report, 8);
    (void)xQueueSend(g_pass_q, &item, 0);
}

void ble_hid_init(void)
{
    if (g_ble_ready) {
        return;
    }

    g_ble_starting = true;

    /* Re-pin if something freed the hold (or early hold failed). */
    if (!g_ble_ram_hold) {
        ble_hid_hold_controller_ram();
    }
    if (!ble_hid_have_controller_ram()) {
        g_ble_starting = false;
        return;
    }

    ble_store_config_init();

    /* Free the early-boot pin immediately before the controller allocates. */
    ble_hid_release_controller_ram();

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        /* Re-pin so a later pairing retry still has a contiguous block. */
        ble_hid_hold_controller_ram();
        g_ble_starting = false;
        return;
    }

    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Register full HOGP before the host task runs. esp_hidd overwrites
     * sync/gatts/reset callbacks — restore sync so we still own advertising. */
    if (ble_hid_gatt_init() != ESP_OK) {
        ESP_LOGE(TAG, "HID GATT setup failed — Bluetooth keyboard unavailable");
        nimble_port_deinit();
        ble_hid_hold_controller_ram();
        g_ble_starting = false;
        return;
    }
    ble_hs_cfg.sync_cb = ble_app_on_sync;

    if (g_pass_q == NULL) {
        g_pass_q = xQueueCreate(BLE_PASS_QUEUE_LEN, sizeof(ble_pass_item_t));
    }
    if (g_pass_q != NULL && g_pass_task == NULL) {
        /* Priority 10: keep Neo→BLE keystrokes ahead of portal/Wi‑Fi workers. */
        if (xTaskCreate(pass_task, "ble_pass", 3072, NULL, 10, &g_pass_task) != pdPASS) {
            ESP_LOGW(TAG, "BLE passthrough task not started");
            g_pass_task = NULL;
        }
    }

    nimble_port_freertos_init(ble_host_task);
    g_ble_ready = true;
    g_ble_starting = false;
    device_status_set_ble(DEVICE_BLE_IDLE);
    ESP_LOGI(TAG, "NimBLE init complete (passthrough + NVS bonds)");
}

static bool ble_hid_nvs_ns_nonempty(const char *ns)
{
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, ns, NVS_TYPE_ANY, &it);
    if (err != ESP_OK || it == NULL) {
        return false;
    }
    nvs_release_iterator(it);
    return true;
}

static void ble_hid_clear_stale_legacy_nvs(void)
{
    static const char *namespaces[] = { "nimble_bond", "bt_nimble" };

    for (size_t i = 0; i < sizeof(namespaces) / sizeof(namespaces[0]); i++) {
        nvs_handle_t h;
        if (nvs_open(namespaces[i], NVS_READWRITE, &h) != ESP_OK) {
            continue;
        }
        if (nvs_erase_all(h) == ESP_OK) {
            (void)nvs_commit(h);
            ESP_LOGI(TAG, "Cleared stale BLE NVS namespace \"%s\"", namespaces[i]);
        }
        nvs_close(h);
    }
}

void ble_hid_boot(void)
{
    uint8_t bonds = ble_hid_load_bond_hint();
    const bool legacy_stale =
        bonds == 0 &&
        (ble_hid_nvs_ns_nonempty("nimble_bond") || ble_hid_nvs_ns_nonempty("bt_nimble"));

    if (legacy_stale) {
        ESP_LOGI(TAG,
                 "Stale NimBLE bond NVS found (0 bonded hosts) — skipping BLE at boot to save RAM");
        ble_hid_clear_stale_legacy_nvs();
        device_status_set_ble(DEVICE_BLE_IDLE);
        return;
    }
    if (bonds == 0) {
        ESP_LOGI(TAG, "BLE idle at boot (no bonded hosts). Start pairing from the portal when needed.");
        device_status_set_ble(DEVICE_BLE_IDLE);
        return;
    }
    ESP_LOGI(TAG, "Starting BLE for reconnect (%u bonded host%s)",
             (unsigned)bonds, bonds == 1 ? "" : "s");
    if (!ble_hid_have_controller_ram()) {
        ESP_LOGW(TAG, "BLE reconnect skipped (low internal RAM after portal start)");
        device_status_set_ble(DEVICE_BLE_IDLE);
        return;
    }
    ble_hid_init();
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
    (void)ble_hid_set_pairing_enabled(false);
    if (g_ble_ready) {
        nimble_port_deinit();
    }
    g_ble_ready = false;
    g_synced = false;
}

static void ble_app_on_sync(void)
{
    ble_hs_util_ensure_addr(0);

    const char *name = settings_get_device_name();
    if (!name || name[0] == '\0') {
        name = "Neo2 Buddy";
    }
    ble_svc_gap_device_name_set(name);

    int bonds = bonded_peer_count();
    ESP_LOGI(TAG, "BLE sync, device name: %s, bonded hosts: %d", name, bonds);
    g_synced = true;
    ble_hid_refresh_bond_hint();

    /* Advertise only for pairing window or existing bonds — never open by default. */
    if (g_pairing_enabled) {
        ble_hid_start_advertising(true);
    } else {
        ble_hid_maybe_advertise();
    }
}

esp_err_t ble_hid_set_pairing_enabled(bool enabled)
{
    if (enabled) {
        g_pairing_enabled = true;
        const bool already_ready = g_ble_ready;
        if (!already_ready) {
            ESP_LOGI(TAG, "Starting BLE stack for pairing");
            /* Quiet window: let SoftAP/httpd settle, keep INTERNAL hold pinned. */
            ble_hid_hold_controller_ram();
            static const int delays_ms[] = { 80, 150, 300, 500, 800 };
            for (size_t attempt = 0;
                 attempt < sizeof(delays_ms) / sizeof(delays_ms[0]) && !g_ble_ready;
                 ++attempt) {
                vTaskDelay(pdMS_TO_TICKS(delays_ms[attempt]));
                if (!g_ble_ram_hold) {
                    ble_hid_hold_controller_ram();
                }
                ble_hid_init();
            }
            /* Wait briefly for host sync so advertising can open this request. */
            for (int i = 0; i < 40 && g_ble_ready && !g_synced; ++i) {
                vTaskDelay(pdMS_TO_TICKS(25));
            }
        }
        if (!g_ble_ready) {
            ESP_LOGE(TAG, "BLE stack failed to start; pairing unavailable");
            g_pairing_enabled = false;
            /* Keep hold for the next attempt — do not leave heap fragmented. */
            ble_hid_hold_controller_ram();
            update_ble_status();
            return ESP_ERR_NO_MEM;
        }

        if (already_ready && g_synced) {
            /* Stack was already up (reconnect ads) — restart open for pairing. */
            if (g_advertising) {
                ble_hid_stop_advertising();
                vTaskDelay(pdMS_TO_TICKS(40));
            }
            ble_hid_start_advertising(true);
        }
        /* Cold start: ble_app_on_sync opens pairing ads when g_pairing_enabled. */

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
        /* Never tear down the NimBLE stack here — keep bonds/reconnect alive.
         * Only close the open pairing window and return to bonded advertising. */
        g_pairing_enabled = false;
        ble_pair_enc_timer_stop();
        ble_pair_sec_timer_stop();
        g_active_peer_valid = false;
        g_active_peer_encrypted = false;
        if (g_pairing_timer) {
            esp_timer_stop(g_pairing_timer);
        }
        ble_hid_stop_advertising();
        if (g_synced) {
            ble_hid_maybe_advertise();
        }
    }
    update_ble_status();
    return ESP_OK;
}

bool ble_hid_pairing_enabled(void)
{
    return g_pairing_enabled;
}

static void ble_hid_maybe_advertise(void)
{
    if (!g_synced) {
        return;
    }
    if (ble_hid_gatt_is_connected()) {
        return;
    }
    if (g_pairing_enabled || bonded_peer_count() > 0) {
        ble_hid_start_advertising(g_pairing_enabled);
    }
}

void ble_hid_start_advertising(bool allow_pairing)
{
    if (!g_synced) {
        ESP_LOGW(TAG, "Ignoring advertise request before BLE sync");
        return;
    }
    if (g_advertising) {
        /* Already advertising — restart if filter policy must change. */
        ble_hid_stop_advertising();
        /* Let the controller finish stop before start (avoids BLE_HS_EALREADY). */
        vTaskDelay(pdMS_TO_TICKS(40));
    }

    struct ble_hs_adv_fields fields = {0};
    const char *name = settings_get_device_name();
    if (!name || name[0] == '\0') {
        name = "Neo2 Buddy";
    }
    /* Keep GAP Device Name in sync (hosts read this after connect). */
    (void)ble_svc_gap_device_name_set(name);

    static ble_uuid16_t adv_uuids16[] = {
        BLE_UUID16_INIT(0x1812),
    };

    /* ADV PDU: flags + appearance + HID UUID + TX power only.
     * Put the full name in the scan response (HijelHID / Combo guidance) so
     * Windows never shows a truncated/garbled name like "Neo2 Buddyw". */
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.appearance = 0x03C1; /* HID Keyboard */
    fields.appearance_is_present = 1;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.uuids16 = adv_uuids16;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set advertising fields: %d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (uint8_t *)name;
    rsp.name_len = (uint8_t)strnlen(name, 29);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGW(TAG, "Scan response name failed: %d", rc);
    }

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.filter_policy = 0;
    /* HID recommended advertising interval (Espressif esp_hid_device). */
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(30);
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(50);

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
    if (rc == BLE_HS_HCI_ERR(BLE_ERR_MEM_CAPACITY)) {
        /* Controller activity slots briefly exhausted (Wi‑Fi coexist / race). */
        vTaskDelay(pdMS_TO_TICKS(80));
        rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                               &adv_params, ble_gap_event, NULL);
    }
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        g_advertising = true;
        if (rc == BLE_HS_EALREADY) {
            ESP_LOGI(TAG, "BLE advertising already active");
        } else {
            ESP_LOGI(TAG, "Started BLE advertising");
        }
    } else {
        ESP_LOGE(TAG, "Failed to start advertising: %d%s", rc,
                 (rc == BLE_HS_HCI_ERR(BLE_ERR_MEM_CAPACITY))
                     ? " (controller MEM_CAPACITY — raise BT_CTRL_BLE_MAX_ACT)"
                     : "");
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
    if (!ble_hid_ensure_preview_buf()) {
        return ESP_ERR_NO_MEM;
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

    if (!text) {
        g_send_running = false;
        g_send_task = NULL;
        vTaskDelete(NULL);
        return;
    }

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
    if (g_preview_text) {
        g_preview_text[0] = '\0';
    }
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
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
                const uint8_t *a = desc.peer_ota_addr.val;
                ESP_LOGI(TAG,
                         "BLE host connected (handle=%d peer=%02x:%02x:%02x:%02x:%02x:%02x type=%u)",
                         event->connect.conn_handle,
                         a[5], a[4], a[3], a[2], a[1], a[0],
                         (unsigned)desc.peer_ota_addr.type);

                g_active_peer = desc.peer_ota_addr;
                g_active_peer_valid = true;
                g_active_peer_encrypted = false;
            } else {
                ESP_LOGI(TAG, "BLE host connected (handle=%d)", event->connect.conn_handle);
                g_active_peer_valid = false;
                g_active_peer_encrypted = false;
            }
            ble_hid_stop_advertising();
            ble_request_low_latency_params(event->connect.conn_handle);
            /* Delay security slightly so Windows can finish GATT discovery. */
            ble_pair_sec_timer_stop();
            g_pair_sec_conn = event->connect.conn_handle;
            if (!g_pair_sec_timer) {
                const esp_timer_create_args_t args = {
                    .callback = pair_sec_delay_cb,
                    .name = "ble_pair_sec",
                };
                esp_timer_create(&args, &g_pair_sec_timer);
            }
            if (g_pair_sec_timer) {
                esp_timer_start_once(g_pair_sec_timer, BLE_PAIR_SEC_DELAY_US);
            }
            if (g_pairing_enabled) {
                if (!g_pair_enc_timer) {
                    const esp_timer_create_args_t args = {
                        .callback = pair_enc_deadline_cb,
                        .name = "ble_pair_enc",
                    };
                    esp_timer_create(&args, &g_pair_enc_timer);
                }
                if (g_pair_enc_timer) {
                    esp_timer_stop(g_pair_enc_timer);
                    esp_timer_start_once(g_pair_enc_timer, BLE_PAIR_ENC_DEADLINE_US);
                }
            }
        } else {
            ESP_LOGI(TAG, "BLE connection failed; status=%d", event->connect.status);
            if (g_pairing_enabled) {
                ble_hid_start_advertising(true);
            } else {
                ble_hid_maybe_advertise();
            }
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT: {
        const int reason = event->disconnect.reason;
        if (reason >= BLE_HS_ERR_HCI_BASE && reason < BLE_HS_ERR_L2C_BASE) {
            ESP_LOGI(TAG, "BLE host disconnected; reason=%d (HCI 0x%02x)",
                     reason, reason - BLE_HS_ERR_HCI_BASE);
        } else {
            ESP_LOGI(TAG, "BLE host disconnected; reason=%d", reason);
        }
        ble_pair_enc_timer_stop();
        ble_pair_sec_timer_stop();
        g_active_peer_valid = false;
        g_active_peer_encrypted = false;
        ble_hid_cancel_send();
        ble_hid_refresh_bond_hint();
        if (g_pairing_enabled) {
            ESP_LOGI(TAG, "Pairing window still open — restarting advertising");
            ble_hid_start_advertising(true);
        } else {
            ble_hid_maybe_advertise();
        }
        break;
    }
    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "Connection params updated; status=%d", event->conn_update.status);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        g_advertising = false;
        if (g_pairing_enabled && !ble_hid_gatt_is_connected()) {
            ble_hid_start_advertising(true);
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "BLE encryption %s",
                 event->enc_change.status == 0 ? "established" : "failed");
        if (event->enc_change.status == 0) {
            g_active_peer_encrypted = true;
            ble_pair_enc_timer_stop();
            ble_hid_refresh_bond_hint();
            /* Re-request after encryption — hosts often accept a shorter interval then. */
            ble_request_low_latency_params(event->enc_change.conn_handle);
            if (g_pairing_enabled) {
                ESP_LOGI(TAG, "Bonding complete — closing pairing window");
                g_pairing_enabled = false;
                if (g_pairing_timer) {
                    esp_timer_stop(g_pairing_timer);
                }
            }
        } else if (g_pairing_enabled) {
            /* Let the host drop (Windows retries). Do not locally terminate —
             * that produced "Try connecting your device again" loops. */
            ESP_LOGW(TAG, "Encryption failed during pairing — waiting for host disconnect");
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        break;
    }

    ble_hid_gatt_gap_event(event);
    update_ble_status();
    return 0;
}
