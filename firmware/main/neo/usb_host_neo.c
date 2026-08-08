/**
 * @file usb_host_neo.c
 * @brief ESP-IDF USB host driver for AlphaSmart Neo2 (VID 0x081E).
 *
 * Detects the Neo in keyboard mode (PID 0xBD04), flips it to comms mode
 * (PID 0xBD01), then exposes bulk read/write used by the neo_* protocol stack.
 *
 * Hard-won rules (do not "simplify" without re-testing on hardware):
 *   1. Neo USB-B needs VBUS 5 V — AAs alone never enumerate (see docs/neo2-usb-wiring.md).
 *   2. Never drop NEW_DEV while busy; queue pending_addr (early firmware lost the Neo here).
 *   3. Separate bulk IN and OUT usb_transfer_t objects; never drain the other direction.
 *   4. Flip only on the USB client task (NEO_USB_ACTION_FLIP) — other tasks request it.
 *   5. Probe devices already on the bus at client start (missed NEW_DEV if Neo was early).
 *   6. NeoTools flip: SET_CONFIGURATION + control outs 0xE0..0xE4; ~4s wait + retry.
 *   7. After BD01, settle before protocol I/O; after backups, RESTART back to HID.
 *
 * Full narrative: firmware/docs/neo-usb-and-backup.md
 */

#include "usb_host_neo.h"

#include "neo_applet.h"
#include "neo_device.h"
#include "neo_file.h"
#include "neo_conv.h"
#include "neo_import.h"
#include "neo_space.h"
#include "neo_live.h"
#include "neo_message.h"
#include "neo_debug.h"
#include "neo_usb_hid.h"
#include "neo_autobackup.h"
#include "board_config.h"
#if HAVE_OLED
#include "display.h"
#endif

#include "cJSON.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "usb_neo";

#define NEO_USB_VID 0x081E
#define NEO_USB_PID_HID 0xBD04
#define NEO_USB_PID_COM 0xBD01
#define NEO_USB_PID_HUB 0x0100

#define NEO_USB_LIB_TASK_STACK 4096
#define NEO_USB_CLIENT_TASK_STACK 8192
#define NEO_USB_TASK_PRIORITY 5
#define NEO_USB_CLIENT_EVENT_MSGS 5
#define NEO_USB_XFER_BUF_SIZE 64
#define NEO_USB_CTRL_BUF_SIZE 64
#define NEO_USB_FLIP_RETRIES 2
#define NEO_USB_FLIP_WAIT_US 4000000LL
#define NEO_USB_MONITOR_MS 10000
#define NEO_USB_IDLE_PROBE_MS 500
#define NEO_USB_RX_STAGING_SIZE 64
/** Neo ASM firmware needs a moment after BD01 enumeration before bulk I/O works. */
#define NEO_USB_COMMS_SETTLE_MS 600
#define NEO_USB_POST_FLIP_MS 400
#define NEO_USB_PROTO_CHUNK 8
/** Extra flip wait after BD01 is seen but setup has not finished yet. */
#define NEO_USB_FLIP_SETUP_GRACE_US 3000000LL

static bool s_host_installed = false;

typedef enum {
    NEO_USB_ACTION_IDLE = 0,
    NEO_USB_ACTION_OPEN,
    NEO_USB_ACTION_CLOSE,
    NEO_USB_ACTION_FLIP,
} neo_usb_action_t;

typedef struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t xfer_done;
    usb_host_client_handle_t client_hdl;
    usb_device_handle_t dev_hdl;
    uint8_t dev_addr;
    uint8_t in_ep;       /**< Bulk IN endpoint address (comms mode). */
    uint8_t out_ep;      /**< Bulk OUT — separate transfer object from IN. */
    uint8_t hid_ep;      /**< Interrupt IN while keyboard_active. */
    uint16_t in_mps;
    uint16_t out_mps;
    uint16_t hid_mps;
    bool ready;          /**< true when 0xBD01 bulk session is open. */
    bool keyboard_active;/**< true when 0xBD04 HID claimed, not flipped. */
    bool flipping;       /**< flip sequence sent, waiting for re-enumeration. */
    int flip_attempt;
    int64_t flip_wait_until_us;
    neo_usb_action_t action; /**< OPEN/CLOSE/FLIP queue for client task. */
    uint8_t pending_addr;  /**< NEW_DEV addr queued if client was busy. */
    usb_transfer_t *bulk_in_xfer;
    usb_transfer_t *bulk_out_xfer;
    usb_transfer_t *ctrl_xfer;
    usb_transfer_t *hid_xfer;
    esp_err_t last_xfer_err;
    neo_usb_dev_info_t last_dev;
    uint8_t rx_staging[NEO_USB_RX_STAGING_SIZE];
    size_t rx_staging_len;
    uint8_t active_xfer_ep;
    volatile bool xfer_pending;      /* control + legacy wait path */
    volatile bool bulk_in_pending;
    volatile bool bulk_out_pending;
} neo_usb_ctx_t;

static neo_usb_ctx_t s_neo = {0};
static TaskHandle_t s_client_task = NULL;
static volatile bool s_rescan_pending = false;
static SemaphoreHandle_t s_rescan_done = NULL;
static SemaphoreHandle_t s_flip_done = NULL;
static SemaphoreHandle_t s_bulk_io_done = NULL;
static SemaphoreHandle_t s_bulk_io_mutex = NULL;
static esp_err_t s_flip_result = ESP_OK;
static neo_usb_scan_result_t s_last_scan = {0};
static int64_t s_last_idle_probe_us = 0;

static void neo_usb_probe_existing(neo_usb_ctx_t *ctx);
static void neo_usb_schedule_open(neo_usb_ctx_t *ctx, uint8_t addr);
static bool neo_usb_pid_is_neo(uint16_t pid);
static void neo_usb_abandon_flip(neo_usb_ctx_t *ctx, const char *reason);
static bool neo_usb_retry_flip_on_hid(neo_usb_ctx_t *ctx);
static void neo_usb_check_flip_timeout(neo_usb_ctx_t *ctx);
static void neo_usb_pump_ms(neo_usb_ctx_t *ctx, int delay_ms);
static void neo_usb_mark_comms_stale(neo_usb_ctx_t *ctx, const char *reason);
static void neo_usb_client_pump(neo_usb_ctx_t *ctx, TickType_t ticks);
static void neo_usb_clear_endpoint(neo_usb_ctx_t *ctx, uint8_t ep_addr);
static void neo_usb_drain_active_xfer(neo_usb_ctx_t *ctx, int timeout_ms);
static void neo_usb_drain_bulk_dir(neo_usb_ctx_t *ctx, bool is_in, int timeout_ms);
static void neo_usb_transfer_cb(usb_transfer_t *transfer);

static esp_err_t neo_usb_bulk_transfer(neo_usb_ctx_t *ctx, uint8_t ep_addr, uint8_t *data, size_t length,
                                       bool is_in, size_t *actual_length, int timeout_ms);

typedef struct {
    volatile bool pending;
    uint8_t ep_addr;
    uint8_t buffer[NEO_USB_XFER_BUF_SIZE];
    size_t length;
    bool is_in;
    /** When true with is_in: fill user_data[0..length) via many USB INs in one client job. */
    bool accum_in;
    int timeout_ms;
    esp_err_t result;
    size_t actual_length;
    uint8_t *user_data;
} neo_usb_bulk_io_t;

static neo_usb_bulk_io_t s_bulk_io;

static usb_transfer_t *neo_usb_bulk_xfer_for(neo_usb_ctx_t *ctx, bool is_in)
{
    return is_in ? ctx->bulk_in_xfer : ctx->bulk_out_xfer;
}

static volatile bool *neo_usb_bulk_pending_for(neo_usb_ctx_t *ctx, bool is_in)
{
    return is_in ? &ctx->bulk_in_pending : &ctx->bulk_out_pending;
}

static esp_err_t neo_usb_bulk_transfer_client(neo_usb_ctx_t *ctx, neo_usb_bulk_io_t *io)
{
    usb_transfer_t *xfer = neo_usb_bulk_xfer_for(ctx, io->is_in);
    volatile bool *pending = neo_usb_bulk_pending_for(ctx, io->is_in);

    if (!ctx->ready || !ctx->dev_hdl || xfer == NULL || io->ep_addr == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (io->length == 0) {
        io->actual_length = 0;
        return ESP_OK;
    }
    if (io->length > xfer->data_buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* Only drain the direction we are about to use — never touch the other
     * transfer object. A stuck IN must not block OUT (and vice versa). */
    neo_usb_drain_bulk_dir(ctx, io->is_in, 300);
    if (*pending) {
        neo_debug_event("bulk %s still in-flight — cannot submit", io->is_in ? "IN" : "OUT");
        return ESP_ERR_NOT_FINISHED;
    }

    xfer->device_handle = ctx->dev_hdl;
    xfer->bEndpointAddress = io->ep_addr;
    xfer->callback = neo_usb_transfer_cb;
    xfer->context = ctx;
    xfer->num_bytes = (int)io->length;
    if (!io->is_in) {
        memcpy(xfer->data_buffer, io->buffer, io->length);
    }

    xSemaphoreTake(ctx->xfer_done, 0);
    *pending = true;
    ctx->active_xfer_ep = io->ep_addr;
    ctx->xfer_pending = true;
    esp_err_t err = usb_host_transfer_submit(xfer);
    if (err != ESP_OK) {
        *pending = false;
        ctx->xfer_pending = false;
        ctx->active_xfer_ep = 0;
        return err;
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(io->timeout_ms > 0 ? io->timeout_ms : 1000);
    while (*pending && xTaskGetTickCount() < deadline) {
        if (xSemaphoreTake(ctx->xfer_done, 0) == pdTRUE) {
            break;
        }
        neo_usb_client_pump(ctx, pdMS_TO_TICKS(5));
    }
    if (*pending) {
        if (ctx->dev_hdl != NULL && io->ep_addr != 0) {
            neo_usb_clear_endpoint(ctx, io->ep_addr);
        }
        neo_usb_drain_bulk_dir(ctx, io->is_in, 500);
        return ESP_ERR_TIMEOUT;
    }
    if (ctx->last_xfer_err == ESP_ERR_INVALID_STATE) {
        neo_usb_mark_comms_stale(ctx, "bulk transfer lost device");
    }
    if (ctx->last_xfer_err != ESP_OK) {
        return ctx->last_xfer_err;
    }
    if (io->is_in) {
        io->actual_length = (size_t)xfer->actual_num_bytes;
        memcpy(io->buffer, xfer->data_buffer, io->actual_length);
    } else {
        io->actual_length = io->length;
    }
    return ESP_OK;
}

/**
 * Pull up to `need` bytes from RX staging or one bulk IN (MPS-sized submit).
 * Neo often returns 8-byte short packets; extras go into rx_staging.
 */
static esp_err_t neo_usb_read_chunk_client(neo_usb_ctx_t *ctx, uint8_t *dest, size_t need, size_t *got,
                                          int timeout_ms)
{
    *got = 0;
    if (need == 0) {
        return ESP_OK;
    }

    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    if (ctx->rx_staging_len > 0) {
        size_t take = need < ctx->rx_staging_len ? need : ctx->rx_staging_len;
        memcpy(dest, ctx->rx_staging, take);
        if (take < ctx->rx_staging_len) {
            memmove(ctx->rx_staging, ctx->rx_staging + take, ctx->rx_staging_len - take);
        }
        ctx->rx_staging_len -= take;
        *got = take;
        xSemaphoreGive(ctx->lock);
        return ESP_OK;
    }
    xSemaphoreGive(ctx->lock);

    size_t mps = ctx->in_mps > 0 ? ctx->in_mps : 64;
    if (mps > NEO_USB_RX_STAGING_SIZE) {
        mps = NEO_USB_RX_STAGING_SIZE;
    }

    neo_usb_bulk_io_t io = {0};
    io.ep_addr = ctx->in_ep;
    io.length = mps;
    io.is_in = true;
    io.timeout_ms = timeout_ms;
    esp_err_t err = neo_usb_bulk_transfer_client(ctx, &io);
    if (err != ESP_OK) {
        return err;
    }
    if (io.actual_length == 0) {
        return ESP_OK;
    }

    size_t take = need < io.actual_length ? need : io.actual_length;
    memcpy(dest, io.buffer, take);
    *got = take;
    if (io.actual_length > take) {
        size_t extra = io.actual_length - take;
        xSemaphoreTake(ctx->lock, portMAX_DELAY);
        if (extra <= NEO_USB_RX_STAGING_SIZE) {
            memcpy(ctx->rx_staging, io.buffer + take, extra);
            ctx->rx_staging_len = extra;
        } else {
            ESP_LOGW(TAG, "RX staging overflow (%u bytes) — dropping tail", (unsigned)extra);
            ctx->rx_staging_len = 0;
        }
        xSemaphoreGive(ctx->lock);
    }
    return ESP_OK;
}

/**
 * Fill dest[0..length) with NeoTools device.read() semantics on the USB client
 * task — one cross-task wait for the whole payload instead of one per 8 bytes.
 */
static esp_err_t neo_usb_read_accum_client(neo_usb_ctx_t *ctx, uint8_t *dest, size_t length, int timeout_ms,
                                           size_t *out_length)
{
    if (!dest || !out_length) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_length = 0;
    if (length == 0) {
        return ESP_OK;
    }
    if (!ctx->ready || ctx->dev_hdl == NULL || ctx->in_ep == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms > 0 ? timeout_ms : 1000);
    while (*out_length < length) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) {
            break;
        }
        int remaining_ms = (int)((deadline - now) * portTICK_PERIOD_MS);
        if (remaining_ms < 1) {
            remaining_ms = 1;
        }
        size_t need = length - *out_length;
        if (need > NEO_USB_PROTO_CHUNK) {
            need = NEO_USB_PROTO_CHUNK;
        }
        size_t chunk = 0;
        esp_err_t err = neo_usb_read_chunk_client(ctx, dest + *out_length, need, &chunk, remaining_ms);
        if (err != ESP_OK) {
            return err;
        }
        if (chunk == 0) {
            break;
        }
        *out_length += chunk;
        if (chunk != NEO_USB_PROTO_CHUNK) {
            break;
        }
    }
    return ESP_OK;
}

static void neo_usb_service_bulk_io(neo_usb_ctx_t *ctx);

static void neo_usb_rx_staging_reset(neo_usb_ctx_t *ctx)
{
    ctx->rx_staging_len = 0;
}

static void neo_usb_clear_endpoint(neo_usb_ctx_t *ctx, uint8_t ep_addr)
{
    if (!ctx->dev_hdl || ep_addr == 0) {
        return;
    }
    ESP_LOGD(TAG, "clear ep=0x%02x", ep_addr);
    (void)usb_host_endpoint_clear(ctx->dev_hdl, ep_addr);
}

static bool neo_usb_is_client_task(void)
{
    return s_client_task != NULL && xTaskGetCurrentTaskHandle() == s_client_task;
}

/** Wake the USB client task so it can pump events (safe from any task). */
static void neo_usb_wake_client(neo_usb_ctx_t *ctx)
{
    if (ctx->client_hdl != NULL) {
        usb_host_client_unblock(ctx->client_hdl);
    }
}

/** Pump USB client events — only call from the usb_neo client task. */
static void neo_usb_client_pump(neo_usb_ctx_t *ctx, TickType_t ticks)
{
    if (!ctx->client_hdl) {
        vTaskDelay(ticks > 0 ? ticks : 1);
        return;
    }
    if (ticks == 0) {
        ticks = 1;
    }
    TickType_t start = xTaskGetTickCount();
    usb_host_client_handle_events(ctx->client_hdl, ticks);
    TickType_t elapsed = xTaskGetTickCount() - start;
    if (elapsed < ticks) {
        vTaskDelay(ticks - elapsed);
    } else {
        vTaskDelay(1);
    }
}

/** Wait for an in-flight bulk/control transfer to finish (or time out). */
static void neo_usb_drain_bulk_dir(neo_usb_ctx_t *ctx, bool is_in, int timeout_ms)
{
    volatile bool *pending = neo_usb_bulk_pending_for(ctx, is_in);
    if (!*pending || ctx->client_hdl == NULL) {
        return;
    }

    uint8_t ep = is_in ? ctx->in_ep : ctx->out_ep;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms > 0 ? timeout_ms : 100);
    while (*pending && xTaskGetTickCount() < deadline) {
        if (xSemaphoreTake(ctx->xfer_done, 0) == pdTRUE && !*pending) {
            return;
        }
        if (neo_usb_is_client_task()) {
            neo_usb_client_pump(ctx, pdMS_TO_TICKS(5));
        } else {
            neo_usb_wake_client(ctx);
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    if (!*pending) {
        return;
    }

    ESP_LOGW(TAG, "xfer still pending on ep=0x%02x — clearing once", ep);
    if (ctx->dev_hdl != NULL && ep != 0) {
        neo_usb_clear_endpoint(ctx, ep);
    }
    TickType_t late_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(200);
    while (*pending && xTaskGetTickCount() < late_deadline) {
        if (neo_usb_is_client_task()) {
            neo_usb_client_pump(ctx, pdMS_TO_TICKS(5));
        } else {
            neo_usb_wake_client(ctx);
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    /* Do NOT forge-clear *pending. ESP-IDF still owns the transfer object until
     * the callback runs; lying here caused ESP_ERR_NOT_FINISHED on the next submit. */
    if (*pending) {
        neo_debug_event("drain: ep=0x%02x still in-flight after clear", ep);
    }
}

static void neo_usb_drain_active_xfer(neo_usb_ctx_t *ctx, int timeout_ms)
{
    neo_usb_drain_bulk_dir(ctx, true, timeout_ms);
    neo_usb_drain_bulk_dir(ctx, false, timeout_ms);
    if (!ctx->xfer_pending || ctx->client_hdl == NULL) {
        return;
    }
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms > 0 ? timeout_ms : 100);
    while (ctx->xfer_pending && xTaskGetTickCount() < deadline) {
        if (xSemaphoreTake(ctx->xfer_done, 0) == pdTRUE) {
            return;
        }
        if (neo_usb_is_client_task()) {
            neo_usb_client_pump(ctx, pdMS_TO_TICKS(5));
        } else {
            neo_usb_wake_client(ctx);
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

static void neo_usb_schedule_reopen_all(neo_usb_ctx_t *ctx)
{
    if (ctx->ready || ctx->dev_hdl != NULL) {
        return;
    }
    uint8_t addrs[8];
    int num_addrs = 0;
    if (usb_host_device_addr_list_fill((int)(sizeof(addrs)), addrs, &num_addrs) != ESP_OK ||
        num_addrs == 0) {
        return;
    }
    for (int i = 0; i < num_addrs; i++) {
        neo_usb_schedule_open(ctx, addrs[i]);
    }
    neo_usb_wake_client(ctx);
}

static void neo_usb_discard_hid_xfer(neo_usb_ctx_t *ctx)
{
    if (ctx->hid_xfer == NULL) {
        return;
    }
    ctx->hid_xfer->device_handle = NULL;
    ctx->hid_xfer->context = NULL;
    ctx->hid_xfer->callback = NULL;
    ctx->hid_xfer = NULL;
    ctx->keyboard_active = false;
}

static bool neo_usb_bus_has_comms_pid(void)
{
    uint8_t addrs[8];
    int num_addrs = 0;
    if (!s_neo.client_hdl ||
        usb_host_device_addr_list_fill((int)(sizeof(addrs)), addrs, &num_addrs) != ESP_OK) {
        return false;
    }
    for (int i = 0; i < num_addrs; i++) {
        usb_device_handle_t probe = NULL;
        if (usb_host_device_open(s_neo.client_hdl, addrs[i], &probe) != ESP_OK) {
            continue;
        }
        const usb_device_desc_t *desc = NULL;
        bool match = usb_host_get_device_descriptor(probe, &desc) == ESP_OK && desc != NULL &&
                     desc->idVendor == NEO_USB_VID && desc->idProduct == NEO_USB_PID_COM;
        usb_host_device_close(s_neo.client_hdl, probe);
        if (match) {
            return true;
        }
    }
    return false;
}

static bool neo_usb_pid_is_neo(uint16_t pid)
{
    return pid == NEO_USB_PID_HID || pid == NEO_USB_PID_COM || pid == NEO_USB_PID_HUB;
}

static void neo_usb_abandon_flip(neo_usb_ctx_t *ctx, const char *reason)
{
    ESP_LOGW(TAG, "Abandoning comms flip: %s", reason);
    neo_debug_event("flip abandoned: %s", reason);
    ctx->flipping = false;
    ctx->flip_attempt = 0;
    ctx->flip_wait_until_us = 0;
    ctx->action = NEO_USB_ACTION_IDLE;
    ctx->pending_addr = 0;
    neo_usb_probe_existing(ctx);
}

static void neo_usb_pump_ms(neo_usb_ctx_t *ctx, int delay_ms)
{
    if (ctx->flipping) {
        neo_usb_check_flip_timeout(ctx);
    }
    int remaining = delay_ms > 0 ? delay_ms : 1;
    while (remaining > 0) {
        int slice = remaining > 10 ? 10 : remaining;
        if (neo_usb_is_client_task()) {
            neo_usb_client_pump(ctx, pdMS_TO_TICKS(slice));
        } else {
            neo_usb_wake_client(ctx);
            vTaskDelay(pdMS_TO_TICKS(slice));
        }
        remaining -= slice;
    }
}

static void neo_usb_mark_comms_stale(neo_usb_ctx_t *ctx, const char *reason)
{
    if (!ctx->ready && ctx->dev_hdl == NULL) {
        return;
    }
    ESP_LOGW(TAG, "Neo comms link lost: %s", reason);
    neo_debug_event("comms stale: %s", reason);
    neo_usb_drain_active_xfer(ctx, 200);
    if (ctx->dev_hdl != NULL) {
        (void)usb_host_interface_release(ctx->client_hdl, ctx->dev_hdl, 0);
        usb_host_device_close(ctx->client_hdl, ctx->dev_hdl);
    }
    ctx->dev_hdl = NULL;
    ctx->dev_addr = 0;
    ctx->ready = false;
    ctx->keyboard_active = false;
    ctx->in_ep = 0;
    ctx->out_ep = 0;
    ctx->xfer_pending = false;
    ctx->bulk_in_pending = false;
    ctx->bulk_out_pending = false;
    ctx->active_xfer_ep = 0;
    neo_usb_rx_staging_reset(ctx);
    neo_usb_schedule_reopen_all(ctx);
}

static bool neo_usb_wait_until_ready(neo_usb_ctx_t *ctx, int iterations, int delay_ms)
{
    for (int i = 0; i < iterations && !ctx->ready; i++) {
        neo_usb_pump_ms(ctx, delay_ms);
    }
    return ctx->ready;
}

static void neo_usb_transfer_cb(usb_transfer_t *transfer)
{
    neo_usb_ctx_t *ctx = (neo_usb_ctx_t *)transfer->context;
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        ctx->last_xfer_err = ESP_OK;
    } else if (transfer->status == USB_TRANSFER_STATUS_TIMED_OUT) {
        ctx->last_xfer_err = ESP_ERR_TIMEOUT;
    } else if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        ctx->last_xfer_err = ESP_ERR_INVALID_STATE;
    } else if (transfer->status == USB_TRANSFER_STATUS_CANCELED) {
        ctx->last_xfer_err = ESP_ERR_TIMEOUT;
    } else {
        /* Do not call neo_debug / ESP_LOG from the transfer callback — it can
         * run while another task holds related locks and previously contributed
         * to Interrupt WDT timeouts. Status is visible via last_xfer_err. */
        ctx->last_xfer_err = ESP_FAIL;
    }
    if (transfer == ctx->bulk_in_xfer) {
        ctx->bulk_in_pending = false;
    } else if (transfer == ctx->bulk_out_xfer) {
        ctx->bulk_out_pending = false;
    }
    ctx->xfer_pending = false;
    ctx->active_xfer_ep = 0;
    xSemaphoreGive(ctx->xfer_done);
}

static void neo_usb_service_bulk_io(neo_usb_ctx_t *ctx)
{
    if (!s_bulk_io.pending) {
        return;
    }
    s_bulk_io.pending = false;
    if (s_bulk_io.accum_in && s_bulk_io.is_in && s_bulk_io.user_data != NULL) {
        s_bulk_io.result = neo_usb_read_accum_client(ctx, s_bulk_io.user_data, s_bulk_io.length,
                                                     s_bulk_io.timeout_ms, &s_bulk_io.actual_length);
    } else {
        s_bulk_io.result = neo_usb_bulk_transfer_client(ctx, &s_bulk_io);
        if (s_bulk_io.is_in && s_bulk_io.user_data != NULL && s_bulk_io.result == ESP_OK) {
            memcpy(s_bulk_io.user_data, s_bulk_io.buffer, s_bulk_io.actual_length);
        }
    }
    xSemaphoreGive(s_bulk_io_done);
}

static esp_err_t neo_usb_wait_transfer(neo_usb_ctx_t *ctx, int timeout_ms)
{
    xSemaphoreTake(ctx->xfer_done, 0);
    if (!neo_usb_is_client_task()) {
        neo_usb_wake_client(ctx);
    }
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms > 0 ? timeout_ms : 1000);
    while (true) {
        if (xSemaphoreTake(ctx->xfer_done, 0) == pdTRUE) {
            if (ctx->last_xfer_err == ESP_FAIL && ctx->dev_hdl != NULL && ctx->active_xfer_ep != 0) {
                neo_usb_clear_endpoint(ctx, ctx->active_xfer_ep);
            }
            return ctx->last_xfer_err;
        }
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) {
            uint8_t ep = ctx->active_xfer_ep;
            TickType_t cleanup_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(100);
            while (xTaskGetTickCount() < cleanup_deadline) {
                if (xSemaphoreTake(ctx->xfer_done, 0) == pdTRUE) {
                    return ctx->last_xfer_err;
                }
                if (neo_usb_is_client_task()) {
                    neo_usb_client_pump(ctx, pdMS_TO_TICKS(10));
                } else {
                    neo_usb_wake_client(ctx);
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
            xSemaphoreTake(ctx->xfer_done, 0);
            if (ctx->dev_hdl != NULL && ep != 0) {
                neo_usb_clear_endpoint(ctx, ep);
            }
            neo_usb_drain_active_xfer(ctx, 500);
            ctx->xfer_pending = false;
            ctx->active_xfer_ep = 0;
            ctx->last_xfer_err = ESP_ERR_TIMEOUT;
            return ESP_ERR_TIMEOUT;
        }
        TickType_t slice = deadline - now;
        if (slice > pdMS_TO_TICKS(10)) {
            slice = pdMS_TO_TICKS(10);
        }
        if (slice < 1) {
            slice = 1;
        }
        if (neo_usb_is_client_task()) {
            neo_usb_client_pump(ctx, slice);
        } else {
            neo_usb_wake_client(ctx);
            vTaskDelay(slice);
        }
    }
}

static esp_err_t neo_usb_submit_control(neo_usb_ctx_t *ctx, int timeout_ms)
{
    ctx->xfer_pending = true;
    esp_err_t err = usb_host_transfer_submit_control(ctx->client_hdl, ctx->ctrl_xfer);
    if (err != ESP_OK) {
        ctx->xfer_pending = false;
        return err;
    }
    return neo_usb_wait_transfer(ctx, timeout_ms);
}

static esp_err_t neo_usb_control_out(neo_usb_ctx_t *ctx, uint8_t bm_request_type, uint8_t b_request,
                                     uint16_t w_value, uint16_t w_index, const uint8_t *out_data,
                                     uint16_t out_length, int timeout_ms)
{
    if (!ctx->dev_hdl || !ctx->ctrl_xfer) {
        return ESP_ERR_INVALID_STATE;
    }

    usb_setup_packet_t *setup = (usb_setup_packet_t *)ctx->ctrl_xfer->data_buffer;
    setup->bmRequestType = bm_request_type;
    setup->bRequest = b_request;
    setup->wValue = w_value;
    setup->wIndex = w_index;
    setup->wLength = out_length;

    if (out_length > 0 && out_data != NULL) {
        if (sizeof(usb_setup_packet_t) + out_length > ctx->ctrl_xfer->data_buffer_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(ctx->ctrl_xfer->data_buffer + sizeof(usb_setup_packet_t), out_data, out_length);
    }

    ctx->ctrl_xfer->device_handle = ctx->dev_hdl;
    ctx->ctrl_xfer->bEndpointAddress = 0;
    ctx->ctrl_xfer->callback = neo_usb_transfer_cb;
    ctx->ctrl_xfer->context = ctx;
    ctx->ctrl_xfer->num_bytes = (int)(sizeof(usb_setup_packet_t) + out_length);
    return neo_usb_submit_control(ctx, timeout_ms);
}

static esp_err_t neo_usb_bulk_transfer(neo_usb_ctx_t *ctx, uint8_t ep_addr, uint8_t *data, size_t length,
                                       bool is_in, size_t *actual_length, int timeout_ms)
{
    usb_transfer_t *xfer = neo_usb_bulk_xfer_for(ctx, is_in);
    if (!ctx->ready || !ctx->dev_hdl || xfer == NULL || ep_addr == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (length == 0) {
        if (actual_length) {
            *actual_length = 0;
        }
        return ESP_OK;
    }
    if (length > xfer->data_buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_bulk_io_mutex || !s_bulk_io_done) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_bulk_io_mutex, portMAX_DELAY);

    esp_err_t err;
    if (neo_usb_is_client_task()) {
        s_bulk_io.ep_addr = ep_addr;
        s_bulk_io.length = length;
        s_bulk_io.is_in = is_in;
        s_bulk_io.accum_in = false;
        s_bulk_io.timeout_ms = timeout_ms;
        s_bulk_io.user_data = is_in ? data : NULL;
        if (!is_in) {
            memcpy(s_bulk_io.buffer, data, length);
        }
        err = neo_usb_bulk_transfer_client(ctx, &s_bulk_io);
        if (err == ESP_OK && is_in) {
            memcpy(data, s_bulk_io.buffer, s_bulk_io.actual_length);
            if (actual_length) {
                *actual_length = s_bulk_io.actual_length;
            }
        } else if (actual_length && err == ESP_OK) {
            *actual_length = length;
        }
        xSemaphoreGive(s_bulk_io_mutex);
        return err;
    }

    xSemaphoreTake(s_bulk_io_done, 0);
    s_bulk_io.ep_addr = ep_addr;
    s_bulk_io.length = length;
    s_bulk_io.is_in = is_in;
    s_bulk_io.accum_in = false;
    s_bulk_io.timeout_ms = timeout_ms;
    s_bulk_io.user_data = is_in ? data : NULL;
    if (!is_in) {
        memcpy(s_bulk_io.buffer, data, length);
    }
    s_bulk_io.pending = true;
    neo_usb_wake_client(ctx);

    int wait_ms = (timeout_ms > 0 ? timeout_ms : 1000) + 3000;
    if (xSemaphoreTake(s_bulk_io_done, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        s_bulk_io.pending = false;
        xSemaphoreGive(s_bulk_io_mutex);
        return ESP_ERR_TIMEOUT;
    }
    err = s_bulk_io.result;
    if (is_in && err == ESP_OK) {
        memcpy(data, s_bulk_io.buffer, s_bulk_io.actual_length);
    }
    if (actual_length) {
        *actual_length = err == ESP_OK ? (is_in ? s_bulk_io.actual_length : length) : 0;
    }
    xSemaphoreGive(s_bulk_io_mutex);
    return err;
}

static bool neo_usb_find_bulk_endpoints(const usb_config_desc_t *config_desc, uint8_t *in_ep, uint8_t *out_ep,
                                        uint16_t *in_mps, uint16_t *out_mps)
{
    int offset = 0;
    const usb_standard_desc_t *current = (const usb_standard_desc_t *)config_desc;
    uint8_t active_interface = 0xFF;

    *in_ep = 0;
    *out_ep = 0;
    *in_mps = 8;
    *out_mps = 8;

    while ((current = usb_parse_next_descriptor(current, config_desc->wTotalLength, &offset)) != NULL) {
        if (current->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)current;
            active_interface = intf->bInterfaceNumber;
        } else if (current->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && active_interface == 0) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)current;
            if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) != USB_BM_ATTRIBUTES_XFER_BULK) {
                continue;
            }
            if (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) {
                *in_ep = ep->bEndpointAddress;
                *in_mps = USB_EP_DESC_GET_MPS(ep);
            } else {
                *out_ep = ep->bEndpointAddress;
                *out_mps = USB_EP_DESC_GET_MPS(ep);
            }
        }
    }
    return *in_ep != 0 && *out_ep != 0;
}

static bool neo_usb_find_interrupt_in(const usb_config_desc_t *config_desc, uint8_t *int_ep, uint16_t *mps)
{
    int offset = 0;
    const usb_standard_desc_t *current = (const usb_standard_desc_t *)config_desc;
    uint8_t active_interface = 0xFF;

    *int_ep = 0;
    *mps = 8;

    while ((current = usb_parse_next_descriptor(current, config_desc->wTotalLength, &offset)) != NULL) {
        if (current->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)current;
            active_interface = intf->bInterfaceNumber;
        } else if (current->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && active_interface == 0) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)current;
            if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) != USB_BM_ATTRIBUTES_XFER_INT) {
                continue;
            }
            if (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) {
                *int_ep = ep->bEndpointAddress;
                *mps = USB_EP_DESC_GET_MPS(ep);
                return true;
            }
        }
    }
    return false;
}

static void neo_usb_hid_transfer_cb(usb_transfer_t *transfer)
{
    neo_usb_ctx_t *ctx = (neo_usb_ctx_t *)transfer->context;
    if (ctx == NULL) {
        return;
    }
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED && transfer->actual_num_bytes > 0) {
        neo_usb_hid_handle_report(transfer->data_buffer, (size_t)transfer->actual_num_bytes);
    } else if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        neo_debug_event("HID xfer status=%d", (int)transfer->status);
    }
    if (ctx->keyboard_active && !ctx->flipping && ctx->hid_xfer == transfer) {
        esp_err_t err = usb_host_transfer_submit(ctx->hid_xfer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "HID resubmit failed: %s", esp_err_to_name(err));
        }
        return;
    }
    if (ctx->hid_xfer == transfer) {
        ctx->hid_xfer = NULL;
        transfer->context = NULL;
        usb_host_transfer_free(transfer);
    }
}

/** Pump USB events until the HID callback frees hid_xfer. Never force-free in-flight. */
static bool neo_usb_drain_hid_xfer(neo_usb_ctx_t *ctx)
{
    if (ctx->hid_xfer == NULL || ctx->client_hdl == NULL) {
        return true;
    }
    for (int i = 0; i < 300 && ctx->hid_xfer != NULL; ++i) {
        if (neo_usb_is_client_task()) {
            neo_usb_client_pump(ctx, pdMS_TO_TICKS(10));
        } else {
            neo_usb_wake_client(ctx);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (ctx->hid_xfer != NULL) {
        ESP_LOGW(TAG, "HID transfer still pending after drain (wait for disconnect callback)");
        return false;
    }
    return true;
}

static esp_err_t neo_usb_hid_start(neo_usb_ctx_t *ctx)
{
    if (!ctx->dev_hdl || ctx->hid_ep == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ctx->hid_xfer != NULL) {
        if (ctx->hid_xfer->device_handle != ctx->dev_hdl) {
            ESP_LOGW(TAG, "Stale HID transfer handle — waiting for teardown");
            ctx->keyboard_active = false;
            if (!neo_usb_drain_hid_xfer(ctx) && ctx->hid_xfer != NULL) {
                return ESP_ERR_INVALID_STATE;
            }
        }
    }
    if (ctx->hid_xfer == NULL) {
        esp_err_t err = usb_host_transfer_alloc(ctx->hid_mps > 0 ? ctx->hid_mps : 8, 0, &ctx->hid_xfer);
        if (err != ESP_OK) {
            return err;
        }
        ctx->hid_xfer->context = ctx;
        ctx->hid_xfer->callback = neo_usb_hid_transfer_cb;
        ctx->hid_xfer->bEndpointAddress = ctx->hid_ep;
        ctx->hid_xfer->device_handle = ctx->dev_hdl;
        ctx->hid_xfer->num_bytes = (int)(ctx->hid_mps > 0 ? ctx->hid_mps : 8);
    }
    ctx->keyboard_active = true;
    neo_usb_hid_reset();
    esp_err_t err = usb_host_transfer_submit(ctx->hid_xfer);
    if (err != ESP_OK) {
        ctx->keyboard_active = false;
        return err;
    }
    ESP_LOGI(TAG, "Neo keyboard listener started (HID IN=0x%02X)", ctx->hid_ep);
    neo_debug_event("keyboard listener started ep=0x%02x", ctx->hid_ep);
    return ESP_OK;
}

static void neo_usb_hid_stop(neo_usb_ctx_t *ctx)
{
    ctx->keyboard_active = false;
    neo_usb_drain_hid_xfer(ctx);
    ctx->hid_ep = 0;
    neo_usb_hid_reset();
}

static esp_err_t neo_usb_setup_keyboard_device(neo_usb_ctx_t *ctx)
{
    const usb_config_desc_t *config_desc = NULL;
    esp_err_t err = usb_host_get_active_config_descriptor(ctx->dev_hdl, &config_desc);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t hid_ep = 0;
    uint16_t hid_mps = 8;
    if (!neo_usb_find_interrupt_in(config_desc, &hid_ep, &hid_mps)) {
        ESP_LOGE(TAG, "HID interrupt IN endpoint not found on Neo keyboard device");
        return ESP_ERR_NOT_FOUND;
    }

    err = usb_host_interface_claim(ctx->client_hdl, ctx->dev_hdl, 0, 0);
    if (err != ESP_OK) {
        return err;
    }

    ctx->hid_ep = hid_ep;
    ctx->hid_mps = hid_mps ? hid_mps : 8;
    return neo_usb_hid_start(ctx);
}

/**
 * NeoTools-compatible HID→comms switch. Must run while the device is still
 * PID 0xBD04 with a claimed interface; device then re-enumerates as 0xBD01.
 */
static esp_err_t neo_usb_flip_to_comms(neo_usb_ctx_t *ctx)
{
    esp_err_t err = neo_usb_control_out(ctx, 0x00, USB_B_REQUEST_SET_CONFIGURATION, 1, 0, NULL, 0, 2000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SET_CONFIGURATION failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Vendor SET_REPORT-style outs; byte sequence taken from NeoTools, not guessed. */
    static const uint8_t magic[] = {0xE0, 0xE1, 0xE2, 0xE3, 0xE4};
    for (size_t i = 0; i < sizeof(magic); i++) {
        err = neo_usb_control_out(ctx, 0x21, 9, 0x0200, 1, &magic[i], 1, 2000);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Comms flip byte 0x%02x failed: %s", magic[i], esp_err_to_name(err));
            neo_debug_event("flip byte 0x%02x failed: %s", magic[i], esp_err_to_name(err));
            return err;
        }
        neo_debug_event("flip byte 0x%02x ok", magic[i]);
    }
    ESP_LOGI(TAG, "Neo HID -> comms flip sequence sent");
    return ESP_OK;
}

static esp_err_t neo_usb_execute_comms_flip(neo_usb_ctx_t *ctx)
{
    if (ctx->ready) {
        return ESP_OK;
    }
    if (ctx->flipping) {
        return ESP_OK;
    }
    if (ctx->dev_hdl == NULL) {
        neo_debug_event("execute_comms_flip: no device handle");
        return ESP_ERR_INVALID_STATE;
    }

    /* Drain interrupt IN before vendor flip control outs on the same interface. */
    neo_usb_hid_stop(ctx);
    ctx->flipping = true;
    ctx->flip_attempt = 1;
    esp_err_t err = neo_usb_flip_to_comms(ctx);
    if (err != ESP_OK) {
        neo_usb_abandon_flip(ctx, esp_err_to_name(err));
        return err;
    }
    ctx->flip_wait_until_us = esp_timer_get_time() + NEO_USB_FLIP_WAIT_US;
    neo_debug_event("flip sequence sent — waiting for PID 0xBD01");
    neo_usb_probe_existing(ctx);
    return ESP_OK;
}

static esp_err_t neo_usb_request_comms_flip(neo_usb_ctx_t *ctx)
{
    if (ctx->ready) {
        return ESP_OK;
    }
    if (ctx->flipping) {
        return ESP_OK;
    }
    if (ctx->dev_hdl == NULL) {
        neo_debug_event("request_comms_flip: no device handle");
        return ESP_ERR_INVALID_STATE;
    }
    if (neo_usb_is_client_task()) {
        return neo_usb_execute_comms_flip(ctx);
    }
    if (ctx->action != NEO_USB_ACTION_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_flip_done == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_flip_done, 0);
    ctx->action = NEO_USB_ACTION_FLIP;
    neo_usb_wake_client(ctx);
    if (xSemaphoreTake(s_flip_done, pdMS_TO_TICKS(15000)) != pdTRUE) {
        ctx->action = NEO_USB_ACTION_IDLE;
        return ESP_ERR_TIMEOUT;
    }
    return s_flip_result;
}

/** NeoTools retries the full flip when comms does not appear in ~4s. */
static bool neo_usb_retry_flip_on_hid(neo_usb_ctx_t *ctx)
{
    uint8_t addrs[8];
    int num_addrs = 0;
    if (usb_host_device_addr_list_fill((int)(sizeof(addrs)), addrs, &num_addrs) != ESP_OK) {
        return false;
    }

    for (int i = 0; i < num_addrs; i++) {
        neo_usb_wake_client(ctx);
        usb_device_handle_t probe = NULL;
        if (usb_host_device_open(ctx->client_hdl, addrs[i], &probe) != ESP_OK) {
            continue;
        }
        const usb_device_desc_t *desc = NULL;
        bool is_hid = usb_host_get_device_descriptor(probe, &desc) == ESP_OK && desc != NULL &&
                      desc->idVendor == NEO_USB_VID && desc->idProduct == NEO_USB_PID_HID;
        if (!is_hid) {
            usb_host_device_close(ctx->client_hdl, probe);
            continue;
        }

        ESP_LOGI(TAG, "Retrying HID->comms flip (attempt %d)", ctx->flip_attempt + 1);
        neo_debug_event("flip retry on HID addr=%u attempt=%d", addrs[i], ctx->flip_attempt + 1);
        ctx->keyboard_active = false;
        esp_err_t err = usb_host_interface_claim(ctx->client_hdl, probe, 0, 0);
        if (err != ESP_OK) {
            usb_host_device_close(ctx->client_hdl, probe);
            continue;
        }
        ctx->dev_hdl = probe;
        ctx->dev_addr = addrs[i];
        err = neo_usb_flip_to_comms(ctx);
        if (err == ESP_OK) {
            ctx->flip_attempt++;
            ctx->flip_wait_until_us = esp_timer_get_time() + NEO_USB_FLIP_WAIT_US;
            neo_usb_probe_existing(ctx);
            return true;
        }
        usb_host_interface_release(ctx->client_hdl, probe, 0);
        usb_host_device_close(ctx->client_hdl, probe);
        ctx->dev_hdl = NULL;
        ctx->dev_addr = 0;
        neo_debug_event("flip retry failed: %s", esp_err_to_name(err));
        return false;
    }
    return false;
}

static void neo_usb_schedule_open(neo_usb_ctx_t *ctx, uint8_t addr)
{
    if ((ctx->ready || ctx->keyboard_active) || addr == 0) {
        return;
    }
    ctx->pending_addr = addr;
    if (ctx->action == NEO_USB_ACTION_IDLE) {
        ctx->action = NEO_USB_ACTION_OPEN;
    }
    if (!neo_usb_is_client_task()) {
        usb_host_client_unblock(ctx->client_hdl);
    }
}

static int neo_usb_log_bus_devices(neo_usb_ctx_t *ctx, uint16_t *vids, uint16_t *pids, int max)
{
    if (!s_host_installed || ctx->client_hdl == NULL) {
        return 0;
    }

    if ((ctx->keyboard_active || ctx->ready) && ctx->dev_hdl != NULL &&
        ctx->last_dev.vendor_id == NEO_USB_VID) {
        if (vids && pids && max > 0) {
            vids[0] = ctx->last_dev.vendor_id;
            pids[0] = ctx->last_dev.product_id;
            return 1;
        }
        return 0;
    }

    uint8_t addrs[8];
    int num_addrs = 0;
    if (usb_host_device_addr_list_fill((int)(sizeof(addrs)), addrs, &num_addrs) != ESP_OK || num_addrs == 0) {
        return 0;
    }

    int logged = 0;
    for (int i = 0; i < num_addrs && logged < max; i++) {
        if (ctx->dev_hdl != NULL && addrs[i] == ctx->dev_addr) {
            continue;
        }
        usb_device_handle_t probe = NULL;
        if (usb_host_device_open(ctx->client_hdl, addrs[i], &probe) != ESP_OK) {
            continue;
        }
        const usb_device_desc_t *desc = NULL;
        if (usb_host_get_device_descriptor(probe, &desc) == ESP_OK && desc != NULL) {
            if (vids && pids) {
                vids[logged] = desc->idVendor;
                pids[logged] = desc->idProduct;
            }
            ESP_LOGI(TAG, "USB device addr=%u VID=0x%04X PID=0x%04X", addrs[i], desc->idVendor,
                     desc->idProduct);
            neo_debug_event("bus dev addr=%u vid=0x%04X pid=0x%04X", addrs[i], desc->idVendor,
                            desc->idProduct);
            logged++;

            if (desc->idVendor == NEO_USB_VID && neo_usb_pid_is_neo(desc->idProduct)) {
                neo_usb_schedule_open(ctx, addrs[i]);
            }
        }
        usb_host_device_close(ctx->client_hdl, probe);
    }
    return logged;
}

static void neo_usb_run_rescan(neo_usb_ctx_t *ctx)
{
    ESP_LOGI(TAG, "Manual USB rescan on OTG1...");
    neo_debug_event("rescan requested");
    memset(&s_last_scan, 0, sizeof(s_last_scan));
    s_last_scan.devices_found =
        neo_usb_log_bus_devices(ctx, s_last_scan.vid, s_last_scan.pid, 4);
    if (!ctx->ready && ctx->action == NEO_USB_ACTION_IDLE) {
        neo_usb_probe_existing(ctx);
    }
    usb_host_lib_info_t info = {0};
    if (usb_host_lib_info(&info) == ESP_OK) {
        s_last_scan.bus_device_count = info.num_devices;
    }
    s_last_scan.neo_ready = ctx->ready;
    s_last_scan.flipping = ctx->flipping;
    if (s_last_scan.bus_device_count == 0) {
        neo_debug_event("rescan: 0 devices — Neo has 5V? check data cable on OTG1");
    } else if (!s_last_scan.neo_ready && !s_last_scan.flipping) {
        neo_debug_event("rescan: %d device(s), starting Neo handshake",
                        s_last_scan.bus_device_count);
    }
}

static void neo_usb_probe_existing(neo_usb_ctx_t *ctx)
{
    if (ctx->ready || ctx->keyboard_active || ctx->dev_hdl != NULL) {
        return;
    }

    uint8_t addrs[8];
    int num_addrs = 0;
    if (usb_host_device_addr_list_fill((int)(sizeof(addrs)), addrs, &num_addrs) != ESP_OK ||
        num_addrs == 0) {
        return;
    }
    ESP_LOGI(TAG, "Checking %d USB device(s) already on the bus", num_addrs);
    if (!neo_usb_is_client_task()) {
        neo_usb_schedule_reopen_all(ctx);
        return;
    }
    for (int i = 0; i < num_addrs && !ctx->ready; i++) {
        usb_device_handle_t probe = NULL;
        esp_err_t open_err = usb_host_device_open(ctx->client_hdl, addrs[i], &probe);
        if (open_err != ESP_OK) {
            ESP_LOGW(TAG, "Probe open addr=%u failed: %s", addrs[i], esp_err_to_name(open_err));
            neo_debug_event("probe open addr=%u failed: %s", addrs[i], esp_err_to_name(open_err));
            neo_usb_schedule_open(ctx, addrs[i]);
            continue;
        }
        const usb_device_desc_t *desc = NULL;
        if (usb_host_get_device_descriptor(probe, &desc) != ESP_OK || desc == NULL) {
            usb_host_device_close(ctx->client_hdl, probe);
            continue;
        }
        if (desc->idVendor != NEO_USB_VID || !neo_usb_pid_is_neo(desc->idProduct)) {
            if (ctx->flipping) {
                ESP_LOGI(TAG, "USB addr=%u VID=0x%04X PID=0x%04X (not Neo, flip pending)",
                         addrs[i], desc->idVendor, desc->idProduct);
                neo_debug_event("probe skip addr=%u vid=0x%04X pid=0x%04X", addrs[i], desc->idVendor,
                                desc->idProduct);
            }
            usb_host_device_close(ctx->client_hdl, probe);
            continue;
        }
        ESP_LOGI(TAG, "Found Neo at USB address %u (PID 0x%04X)", addrs[i], desc->idProduct);
        neo_debug_event("probe found Neo addr=%u pid=0x%04X", addrs[i], desc->idProduct);
        usb_host_device_close(ctx->client_hdl, probe);
        neo_usb_schedule_open(ctx, addrs[i]);
        return;
    }
}

static void neo_usb_check_flip_timeout(neo_usb_ctx_t *ctx)
{
    if (!ctx->flipping || ctx->ready || ctx->flip_wait_until_us == 0) {
        return;
    }
    if (esp_timer_get_time() < ctx->flip_wait_until_us) {
        return;
    }

    neo_usb_probe_existing(ctx);
    if (ctx->ready) {
        return;
    }

    if (ctx->action == NEO_USB_ACTION_OPEN || ctx->pending_addr != 0 || neo_usb_bus_has_comms_pid()) {
        ctx->flip_wait_until_us = esp_timer_get_time() + NEO_USB_FLIP_SETUP_GRACE_US;
        neo_debug_event("flip: BD01/setup in progress, extending wait");
        return;
    }

    if (ctx->flip_attempt < NEO_USB_FLIP_RETRIES && neo_usb_retry_flip_on_hid(ctx)) {
        ESP_LOGW(TAG, "Neo comms slow to appear, retried flip (attempt %d/%d)", ctx->flip_attempt,
                 NEO_USB_FLIP_RETRIES);
        return;
    }

    ESP_LOGE(TAG, "Timed out waiting for Neo comms (PID 0xBD01) — restoring USB scan");
    neo_usb_abandon_flip(ctx, "comms timeout");
}

static void neo_usb_clear_connection(neo_usb_ctx_t *ctx)
{
    neo_usb_hid_stop(ctx);
    ctx->ready = false;
    ctx->keyboard_active = false;
    ctx->dev_hdl = NULL;
    ctx->dev_addr = 0;
    ctx->in_ep = 0;
    ctx->out_ep = 0;
    neo_usb_rx_staging_reset(ctx);
}

static esp_err_t neo_usb_setup_comms_device(neo_usb_ctx_t *ctx)
{
    const usb_device_desc_t *dev_desc = NULL;
    esp_err_t err = usb_host_get_device_descriptor(ctx->dev_hdl, &dev_desc);
    if (err != ESP_OK || dev_desc == NULL) {
        return err != ESP_OK ? err : ESP_FAIL;
    }

    if (dev_desc->idVendor != NEO_USB_VID || dev_desc->idProduct != NEO_USB_PID_COM) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    err = neo_usb_control_out(ctx, 0x00, USB_B_REQUEST_SET_CONFIGURATION, 1, 0, NULL, 0, 2000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "comms SET_CONFIGURATION: %s (continuing)", esp_err_to_name(err));
    }

    const usb_config_desc_t *config_desc = NULL;
    err = usb_host_get_active_config_descriptor(ctx->dev_hdl, &config_desc);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t in_ep = 0;
    uint8_t out_ep = 0;
    uint16_t in_mps = 8;
    uint16_t out_mps = 8;
    if (!neo_usb_find_bulk_endpoints(config_desc, &in_ep, &out_ep, &in_mps, &out_mps)) {
        ESP_LOGE(TAG, "Bulk endpoints not found on Neo comms device");
        return ESP_ERR_NOT_FOUND;
    }

    err = usb_host_interface_claim(ctx->client_hdl, ctx->dev_hdl, 0, 0);
    if (err != ESP_OK) {
        return err;
    }

    ctx->in_ep = in_ep;
    ctx->out_ep = out_ep;
    ctx->in_mps = in_mps ? in_mps : 8;
    ctx->out_mps = out_mps ? out_mps : 8;
    neo_usb_rx_staging_reset(ctx);
    for (int i = 0; i < (NEO_USB_COMMS_SETTLE_MS / 100); i++) {
        neo_usb_pump_ms(ctx, 100);
    }
    ctx->ready = true;
    ctx->flipping = false;
    ctx->flip_attempt = 0;
    ctx->flip_wait_until_us = 0;

    ctx->last_dev.vendor_id = dev_desc->idVendor;
    ctx->last_dev.product_id = dev_desc->idProduct;
    snprintf(ctx->last_dev.product_string, sizeof(ctx->last_dev.product_string), "AlphaSmart Neo2");

    ESP_LOGI(TAG, "Neo comms ready (VID=0x%04X PID=0x%04X IN=0x%02X OUT=0x%02X MPS in=%u out=%u)",
             dev_desc->idVendor, dev_desc->idProduct, in_ep, out_ep, (unsigned)ctx->in_mps,
             (unsigned)ctx->out_mps);
    neo_debug_event("comms ready in=0x%02x out=0x%02x mps=%u/%u", in_ep, out_ep, (unsigned)ctx->in_mps,
                    (unsigned)ctx->out_mps);

    /* Do not prime IN here. NeoTools starts with hello OUT; a speculative IN
     * leaves the IN transfer object in-flight and historically blocked OUT when
     * both directions shared one usb_transfer_t. */
    return ESP_OK;
}

static void neo_usb_process_open(neo_usb_ctx_t *ctx)
{
    uint8_t addr = ctx->pending_addr;
    ctx->pending_addr = 0;
    ctx->action = NEO_USB_ACTION_IDLE;

    if (addr == 0) {
        return;
    }

    ESP_LOGI(TAG, "Opening USB device at addr %u...", addr);
    neo_debug_event("open addr=%u start", addr);

    esp_err_t err = usb_host_device_open(ctx->client_hdl, addr, &ctx->dev_hdl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not open USB device at addr %u: %s", addr, esp_err_to_name(err));
        neo_debug_event("open addr=%u failed: %s", addr, esp_err_to_name(err));
        return;
    }
    ctx->dev_addr = addr;

    const usb_device_desc_t *dev_desc = NULL;
    err = usb_host_get_device_descriptor(ctx->dev_hdl, &dev_desc);
    if (err != ESP_OK || dev_desc == NULL) {
        ESP_LOGW(TAG, "Device descriptor unavailable at addr %u: %s", addr,
                 err != ESP_OK ? esp_err_to_name(err) : "null");
        neo_debug_event("desc addr=%u failed: %s", addr,
                        err != ESP_OK ? esp_err_to_name(err) : "null");
        usb_host_device_close(ctx->client_hdl, ctx->dev_hdl);
        neo_usb_clear_connection(ctx);
        return;
    }

    ESP_LOGI(TAG, "USB device addr=%u VID=0x%04X PID=0x%04X", addr, dev_desc->idVendor,
             dev_desc->idProduct);
    neo_debug_event("dev addr=%u vid=0x%04X pid=0x%04X", addr, dev_desc->idVendor,
                    dev_desc->idProduct);

    if (dev_desc->idVendor != NEO_USB_VID) {
        ESP_LOGI(TAG, "Not a Neo (expected VID 0x%04X) — ignoring", NEO_USB_VID);
        usb_host_device_close(ctx->client_hdl, ctx->dev_hdl);
        neo_usb_clear_connection(ctx);
        return;
    }

    ctx->last_dev.vendor_id = dev_desc->idVendor;
    ctx->last_dev.product_id = dev_desc->idProduct;
    snprintf(ctx->last_dev.product_string, sizeof(ctx->last_dev.product_string), "AlphaSmart Neo2");

    if (dev_desc->idProduct == NEO_USB_PID_HUB) {
        ESP_LOGI(TAG, "Neo USB hub stage (PID 0x0100) — waiting for HID/comms device...");
        neo_debug_event("Neo hub PID 0x0100 — waiting for re-enumeration");
        usb_host_device_close(ctx->client_hdl, ctx->dev_hdl);
        neo_usb_clear_connection(ctx);
        return;
    }

    if (dev_desc->idProduct == NEO_USB_PID_HID) {
        if (ctx->flipping) {
            ESP_LOGI(TAG, "Neo still in HID mode during flip — waiting for comms PID 0xBD01");
            neo_debug_event("flip: HID at addr=%u, waiting for comms", addr);
            usb_host_device_close(ctx->client_hdl, ctx->dev_hdl);
            ctx->dev_hdl = NULL;
            ctx->dev_addr = 0;
            return;
        }
        /* Claim interrupt IN and listen for boot keyboard reports (UART + live + portal raw).
         * Comms flip stays on-demand (ensure_comms / autobackup), not automatic on every plug. */
        err = neo_usb_setup_keyboard_device(ctx);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "HID keyboard setup failed: %s", esp_err_to_name(err));
            neo_debug_event("HID keyboard setup failed: %s", esp_err_to_name(err));
            usb_host_device_close(ctx->client_hdl, ctx->dev_hdl);
            neo_usb_clear_connection(ctx);
            return;
        }
        ESP_LOGI(TAG, "Neo keyboard mode (PID 0xBD04) — HID listening, comms on demand");
        neo_debug_event("HID mode addr=%u — listening, comms on demand", addr);
        neo_autobackup_on_keyboard_connected();
        return;
    }

    if (dev_desc->idProduct == NEO_USB_PID_COM) {
        err = neo_usb_setup_comms_device(ctx);
        if (err != ESP_OK) {
            if (ctx->dev_hdl) {
                usb_host_device_close(ctx->client_hdl, ctx->dev_hdl);
            }
            neo_usb_clear_connection(ctx);
            if (ctx->flipping) {
                neo_usb_abandon_flip(ctx, "comms setup failed");
            }
            ESP_LOGE(TAG, "Neo comms setup failed: %s", esp_err_to_name(err));
            neo_debug_event("comms setup failed: %s", esp_err_to_name(err));
        }
        return;
    }

    ESP_LOGW(TAG, "Unknown Neo PID 0x%04X — expected 0xBD04 or 0xBD01", dev_desc->idProduct);
    neo_debug_event("unknown Neo pid=0x%04X", dev_desc->idProduct);
    usb_host_device_close(ctx->client_hdl, ctx->dev_hdl);
    neo_usb_clear_connection(ctx);
}

static void neo_usb_process_close(neo_usb_ctx_t *ctx)
{
    bool flipping = ctx->flipping;

    if (ctx->dev_hdl != NULL) {
        (void)usb_host_interface_release(ctx->client_hdl, ctx->dev_hdl, 0);
        usb_host_device_close(ctx->client_hdl, ctx->dev_hdl);
    }
    ctx->keyboard_active = false;
    (void)neo_usb_drain_hid_xfer(ctx);
    ctx->dev_hdl = NULL;
    ctx->dev_addr = 0;
    ctx->ready = false;
    ctx->in_ep = 0;
    ctx->out_ep = 0;
    ctx->hid_ep = 0;
    ctx->xfer_pending = false;
    ctx->bulk_in_pending = false;
    ctx->bulk_out_pending = false;
    ctx->active_xfer_ep = 0;
    neo_usb_rx_staging_reset(ctx);
    neo_usb_hid_reset();
    if (!flipping) {
        ctx->flipping = false;
        ctx->flip_attempt = 0;
        ctx->flip_wait_until_us = 0;
    }
    ESP_LOGI(TAG, "Neo USB disconnected%s", flipping ? " (re-enumeration expected)" : "");
    neo_debug_event("USB disconnected%s", flipping ? " (flip pending)" : "");
    ctx->action = NEO_USB_ACTION_IDLE;
    if (flipping && !ctx->ready) {
        neo_usb_probe_existing(ctx);
    }
}

static void neo_usb_client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    neo_usb_ctx_t *ctx = (neo_usb_ctx_t *)arg;
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        neo_debug_event("USB NEW_DEV addr=%u", event_msg->new_dev.address);
        neo_usb_schedule_open(ctx, event_msg->new_dev.address);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        neo_debug_event("USB DEV_GONE");
        ESP_LOGI(TAG, "USB device disconnected from bus");
        neo_usb_discard_hid_xfer(ctx);
        if (ctx->dev_hdl != NULL && event_msg->dev_gone.dev_hdl == ctx->dev_hdl) {
            ctx->action = NEO_USB_ACTION_CLOSE;
            usb_host_client_unblock(ctx->client_hdl);
        } else if (ctx->flipping && !ctx->ready) {
            ctx->pending_addr = 0;
            neo_usb_probe_existing(ctx);
            usb_host_client_unblock(ctx->client_hdl);
        } else if (!ctx->ready) {
            /* Device left before we opened it — retry probe shortly. */
            ctx->pending_addr = 0;
            usb_host_client_unblock(ctx->client_hdl);
        }
        break;
    default:
        break;
    }
}

static void neo_usb_client_task(void *arg)
{
    neo_usb_ctx_t *ctx = (neo_usb_ctx_t *)arg;
    s_client_task = xTaskGetCurrentTaskHandle();

    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = NEO_USB_CLIENT_EVENT_MSGS,
        .async = {
            .client_event_callback = neo_usb_client_event_cb,
            .callback_arg = ctx,
        },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&client_config, &ctx->client_hdl));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(NEO_USB_XFER_BUF_SIZE, 0, &ctx->bulk_in_xfer));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(NEO_USB_XFER_BUF_SIZE, 0, &ctx->bulk_out_xfer));
    ctx->bulk_in_xfer->context = ctx;
    ctx->bulk_out_xfer->context = ctx;
    ESP_ERROR_CHECK(usb_host_transfer_alloc(NEO_USB_CTRL_BUF_SIZE, 0, &ctx->ctrl_xfer));
    ctx->ctrl_xfer->context = ctx;

    ESP_LOGI(TAG, "USB Neo client task running (stack v16: separate IN/OUT bulk xfers)");
    vTaskDelay(pdMS_TO_TICKS(50));
    neo_usb_probe_existing(ctx);

    while (true) {
        neo_usb_service_bulk_io(ctx);
        neo_usb_check_flip_timeout(ctx);
        if (s_rescan_pending) {
            s_rescan_pending = false;
            neo_usb_run_rescan(ctx);
            if (s_rescan_done) {
                xSemaphoreGive(s_rescan_done);
            }
        }
        if (ctx->action == NEO_USB_ACTION_FLIP) {
            ctx->action = NEO_USB_ACTION_IDLE;
            xSemaphoreTake(ctx->lock, portMAX_DELAY);
            s_flip_result = neo_usb_execute_comms_flip(ctx);
            xSemaphoreGive(ctx->lock);
            if (s_flip_done != NULL) {
                xSemaphoreGive(s_flip_done);
            }
        } else if (ctx->action == NEO_USB_ACTION_OPEN) {
            xSemaphoreTake(ctx->lock, portMAX_DELAY);
            neo_usb_process_open(ctx);
            xSemaphoreGive(ctx->lock);
        } else if (ctx->action == NEO_USB_ACTION_CLOSE) {
            xSemaphoreTake(ctx->lock, portMAX_DELAY);
            neo_usb_process_close(ctx);
            xSemaphoreGive(ctx->lock);
        } else if (ctx->pending_addr != 0 && !ctx->ready && !ctx->keyboard_active) {
            ctx->action = NEO_USB_ACTION_OPEN;
        } else if (!ctx->ready && !ctx->keyboard_active) {
            int64_t now_us = esp_timer_get_time();
            if ((now_us - s_last_idle_probe_us) >= ((int64_t)NEO_USB_IDLE_PROBE_MS * 1000)) {
                s_last_idle_probe_us = now_us;
                neo_usb_probe_existing(ctx);
            }
        }
        TickType_t slice = (ctx->xfer_pending || ctx->bulk_in_pending || ctx->bulk_out_pending ||
                            s_bulk_io.pending)
                               ? pdMS_TO_TICKS(5)
                               : pdMS_TO_TICKS(50);
        neo_usb_client_pump(ctx, slice);
    }
}

static void neo_usb_lib_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void neo_usb_monitor_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));
    int last_devices = -1;
    bool last_ready = false;
    bool last_keyboard = false;
    bool last_flipping = false;
    bool have_last = false;
    while (true) {
        usb_host_lib_info_t info = {0};
        if (s_host_installed && usb_host_lib_info(&info) == ESP_OK) {
            bool changed = !have_last || info.num_devices != last_devices || s_neo.ready != last_ready ||
                           s_neo.keyboard_active != last_keyboard || s_neo.flipping != last_flipping;
            bool verbose = neo_debug_is_verbose();

            if (info.num_devices == 0 && !s_neo.ready && !s_neo.flipping) {
                if (changed || verbose) {
                    ESP_LOGI(TAG, "USB host OK, 0 devices — Neo2 not seen on OTG1 yet");
                    if (verbose) {
                        ESP_LOGI(TAG, "  Check: Neo2 USB-B has 5V (AAs alone won't wake USB), data via OTG1");
                    }
                    neo_debug_event("monitor: 0 USB devices on OTG1 (Neo 5V? data cable?)");
                }
            } else if (changed || verbose) {
                ESP_LOGI(TAG, "USB bus: %d device(s), neo_ready=%d keyboard=%d flipping=%d",
                         info.num_devices, s_neo.ready, s_neo.keyboard_active, s_neo.flipping);
                neo_debug_event("monitor: bus_dev=%d neo_ready=%d keyboard=%d flipping=%d",
                                info.num_devices, s_neo.ready, s_neo.keyboard_active, s_neo.flipping);
                if (!s_neo.ready && s_neo.keyboard_active && s_neo.last_dev.vendor_id == NEO_USB_VID) {
                    ESP_LOGI(TAG, "  -> AlphaSmart Neo in keyboard mode (PID 0x%04X), comms on demand",
                             s_neo.last_dev.product_id);
                } else if (!s_neo.ready && s_neo.client_hdl != NULL && !s_neo.keyboard_active &&
                           s_neo.dev_hdl == NULL) {
                    uint16_t vids[4] = {0};
                    uint16_t pids[4] = {0};
                    int n = neo_usb_log_bus_devices(&s_neo, vids, pids, 4);
                    for (int i = 0; i < n; i++) {
                        if (vids[i] == NEO_USB_VID) {
                            ESP_LOGI(TAG, "  -> AlphaSmart Neo detected (PID 0x%04X)%s", pids[i],
                                     s_neo.flipping ? ", comms flip pending" : ", handshake pending");
                        } else if (verbose) {
                            ESP_LOGI(TAG, "  -> device[%d] VID=0x%04X PID=0x%04X (not Neo — unplug for Neo test)",
                                     i, vids[i], pids[i]);
                        }
                    }
                }
            }

            if (!s_neo.ready && s_neo.client_hdl != NULL && !s_neo.keyboard_active && s_neo.dev_hdl == NULL &&
                s_neo.flipping && !s_neo.ready) {
                neo_usb_probe_existing(&s_neo);
            }
            if (info.num_devices == 0 && !s_neo.ready && s_neo.client_hdl != NULL) {
                if (s_neo.flipping) {
                    if (esp_timer_get_time() < s_neo.flip_wait_until_us) {
                        neo_debug_event("monitor: 0 devices during flip (re-enumeration expected)");
                        neo_usb_probe_existing(&s_neo);
                    } else {
                        neo_usb_abandon_flip(&s_neo, "flip timeout with no device");
                    }
                } else {
                    neo_usb_probe_existing(&s_neo);
                }
            }

            last_devices = info.num_devices;
            last_ready = s_neo.ready;
            last_keyboard = s_neo.keyboard_active;
            last_flipping = s_neo.flipping;
            have_last = true;
        }
        vTaskDelay(pdMS_TO_TICKS(NEO_USB_MONITOR_MS));
    }
}

void usb_host_neo_get_host_status(neo_usb_host_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->host_installed = s_host_installed;
    out->neo_ready = s_neo.ready;
    out->keyboard_active = s_neo.keyboard_active;
    out->flipping = s_neo.flipping;
    out->flip_attempt = s_neo.flip_attempt;
    out->last_vid = s_neo.last_dev.vendor_id;
    out->last_pid = s_neo.last_dev.product_id;
    if (s_host_installed) {
        usb_host_lib_info_t info = {0};
        if (usb_host_lib_info(&info) == ESP_OK) {
            out->bus_device_count = info.num_devices;
        }
    }
}

esp_err_t usb_host_neo_rescan(neo_usb_scan_result_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!s_host_installed || s_neo.client_hdl == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_rescan_done) {
        xSemaphoreTake(s_rescan_done, 0);
    }
    s_rescan_pending = true;
    usb_host_client_unblock(s_neo.client_hdl);

    if (s_rescan_done) {
        xSemaphoreTake(s_rescan_done, pdMS_TO_TICKS(3000));
    } else {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    *out = s_last_scan;
    out->neo_ready = s_neo.ready;
    out->flipping = s_neo.flipping;
    usb_host_lib_info_t info = {0};
    if (usb_host_lib_info(&info) == ESP_OK) {
        out->bus_device_count = info.num_devices;
    }
    return ESP_OK;
}

esp_err_t usb_host_neo_init(void)
{
    memset(&s_neo, 0, sizeof(s_neo));
    s_neo.lock = xSemaphoreCreateMutex();
    s_neo.xfer_done = xSemaphoreCreateBinary();
    s_rescan_done = xSemaphoreCreateBinary();
    s_flip_done = xSemaphoreCreateBinary();
    s_bulk_io_done = xSemaphoreCreateBinary();
    s_bulk_io_mutex = xSemaphoreCreateMutex();
    if (!s_neo.lock || !s_neo.xfer_done || !s_rescan_done || !s_flip_done || !s_bulk_io_done ||
        !s_bulk_io_mutex) {
        return ESP_ERR_NO_MEM;
    }

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        neo_debug_event("usb_host_install FAILED: %s", esp_err_to_name(err));
        return err;
    }
    s_host_installed = true;

    BaseType_t created = xTaskCreate(neo_usb_lib_task, "usb_lib", NEO_USB_LIB_TASK_STACK, NULL,
                                     NEO_USB_TASK_PRIORITY, NULL);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    created = xTaskCreate(neo_usb_client_task, "usb_neo", NEO_USB_CLIENT_TASK_STACK, &s_neo,
                          NEO_USB_TASK_PRIORITY + 1, NULL);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    created = xTaskCreate(neo_usb_monitor_task, "usb_mon", 3072, NULL, 3, NULL);
    if (created != pdPASS) {
        ESP_LOGW(TAG, "USB monitor task not started");
    }

    ESP_LOGW(TAG, "=== Neo2 USB: use OTG1 port on the Olimex board ===");
    ESP_LOGW(TAG, "Neo2 USB-B needs 5V for emulation (powerbank OK; internal AAs not enough alone)");
    ESP_LOGW(TAG, "Data: USB-C into OTG1. Serial monitor uses the other USB-C port.");
    ESP_LOGW(TAG, "Split power/data wiring: docs/neo2-usb-wiring.md");
    neo_debug_event("USB host ready on OTG1");
    return ESP_OK;
}

bool usb_host_neo_is_connected(void)
{
    return s_neo.ready || s_neo.keyboard_active;
}

/**
 * Make NeoTools bulk protocol available. Single choke-point for web/CLI/backup.
 *
 * Order matters: reuse an open BD01 session when healthy; wait out an in-flight
 * flip; reopen if BD01 is already on the bus; only then start a HID flip.
 * Success path stays quiet (failures still WARN / ring) so serial stays usable.
 */
esp_err_t usb_host_neo_ensure_comms(void)
{
    if (s_neo.ready) {
        if (s_neo.dev_hdl == NULL || s_neo.in_ep == 0 || s_neo.out_ep == 0) {
            neo_debug_event("ensure_comms stale session — reopening");
            neo_usb_mark_comms_stale(&s_neo, "missing device or endpoints");
        } else {
            return ESP_OK;
        }
    }
    if (s_neo.flipping) {
        if (neo_usb_wait_until_ready(&s_neo, 80, 100)) {
            return ESP_OK;
        }
        neo_debug_event("ensure_comms flip wait timeout");
        neo_usb_abandon_flip(&s_neo, "ensure_comms timeout");
        return ESP_ERR_TIMEOUT;
    }
    if (!s_neo.keyboard_active) {
        if (neo_usb_bus_has_comms_pid()) {
            neo_usb_schedule_reopen_all(&s_neo);
            if (neo_usb_wait_until_ready(&s_neo, 50, 100)) {
                neo_usb_pump_ms(&s_neo, NEO_USB_POST_FLIP_MS);
                return ESP_OK;
            }
        }
        neo_usb_probe_existing(&s_neo);
        neo_usb_pump_ms(&s_neo, 100);
        for (int i = 0; i < 80 && !s_neo.ready && !s_neo.flipping; i++) {
            neo_usb_pump_ms(&s_neo, 100);
        }
        if (s_neo.ready) {
            return ESP_OK;
        }
        if (s_neo.flipping) {
            return usb_host_neo_ensure_comms();
        }
        neo_debug_event("ensure_comms failed: Neo not in keyboard mode");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t flip_err = ESP_ERR_INVALID_STATE;
    if (s_neo.keyboard_active && s_neo.dev_hdl != NULL) {
        flip_err = neo_usb_request_comms_flip(&s_neo);
    }
    if (flip_err != ESP_OK) {
        neo_debug_event("ensure_comms flip start failed: %s", esp_err_to_name(flip_err));
        return flip_err;
    }
    if (neo_usb_wait_until_ready(&s_neo, 80, 100)) {
        neo_usb_pump_ms(&s_neo, NEO_USB_POST_FLIP_MS);
        return ESP_OK;
    }
    neo_debug_event("ensure_comms flip timeout");
    neo_usb_abandon_flip(&s_neo, "ensure_comms timeout");
    return ESP_ERR_TIMEOUT;
}

bool usb_host_neo_is_comms_ready(void)
{
    return s_neo.ready;
}

void usb_host_neo_publish_keyboard_text(const char *text, size_t length)
{
    neo_live_append(text, length);
#if HAVE_OLED
    display_request_home();
#endif
}

void usb_host_neo_prepare_dialogue(void)
{
    if (!s_neo.ready || s_neo.dev_hdl == NULL) {
        return;
    }
    /* Brief drain only — NeoTools does not sleep before hello. */
    neo_usb_drain_active_xfer(&s_neo, 80);
}

void usb_host_neo_recover_transport(void)
{
    if (!s_neo.ready || s_neo.dev_hdl == NULL) {
        return;
    }
    neo_usb_drain_active_xfer(&s_neo, 500);
    uint8_t in_ep = s_neo.in_ep;
    uint8_t out_ep = s_neo.out_ep;
    if (in_ep != 0) {
        neo_usb_clear_endpoint(&s_neo, in_ep);
    }
    if (out_ep != 0) {
        neo_usb_clear_endpoint(&s_neo, out_ep);
    }
    xSemaphoreTake(s_neo.lock, portMAX_DELAY);
    neo_usb_rx_staging_reset(&s_neo);
    xSemaphoreGive(s_neo.lock);
    neo_usb_pump_ms(&s_neo, 150);
}

void usb_host_neo_invalidate_comms(void)
{
    neo_usb_mark_comms_stale(&s_neo, "protocol session reset");
}

esp_err_t usb_host_neo_read(uint8_t *buffer, size_t capacity, int timeout_ms, size_t *out_length)
{
    if (!buffer || capacity == 0 || !out_length) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_neo.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (timeout_ms <= 0) {
        timeout_ms = 1000;
    }
    if (!s_bulk_io_mutex || !s_bulk_io_done) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_length = 0;

    /* Satisfy entirely from staging without waking the client when possible. */
    xSemaphoreTake(s_neo.lock, portMAX_DELAY);
    if (s_neo.rx_staging_len >= capacity) {
        memcpy(buffer, s_neo.rx_staging, capacity);
        memmove(s_neo.rx_staging, s_neo.rx_staging + capacity, s_neo.rx_staging_len - capacity);
        s_neo.rx_staging_len -= capacity;
        *out_length = capacity;
        xSemaphoreGive(s_neo.lock);
        neo_debug_xfer("usb_in(staged)", ESP_OK, buffer, capacity);
        return ESP_OK;
    }
    size_t pre = 0;
    if (s_neo.rx_staging_len > 0) {
        pre = s_neo.rx_staging_len;
        memcpy(buffer, s_neo.rx_staging, pre);
        s_neo.rx_staging_len = 0;
        *out_length = pre;
    }
    xSemaphoreGive(s_neo.lock);

    size_t need = capacity - pre;
    if (need == 0) {
        neo_debug_xfer("usb_in(staged)", ESP_OK, buffer, pre);
        return ESP_OK;
    }

    /* One client-task job fills the rest (NeoTools 8-byte short-packet loop). */
    esp_err_t err;
    size_t got = 0;
    xSemaphoreTake(s_bulk_io_mutex, portMAX_DELAY);
    if (neo_usb_is_client_task()) {
        err = neo_usb_read_accum_client(&s_neo, buffer + pre, need, timeout_ms, &got);
        xSemaphoreGive(s_bulk_io_mutex);
    } else {
        xSemaphoreTake(s_bulk_io_done, 0);
        memset(&s_bulk_io, 0, sizeof(s_bulk_io));
        s_bulk_io.ep_addr = s_neo.in_ep;
        s_bulk_io.length = need;
        s_bulk_io.is_in = true;
        s_bulk_io.accum_in = true;
        s_bulk_io.timeout_ms = timeout_ms;
        s_bulk_io.user_data = buffer + pre;
        s_bulk_io.pending = true;
        neo_usb_wake_client(&s_neo);

        int wait_ms = timeout_ms + 3000;
        if (xSemaphoreTake(s_bulk_io_done, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
            s_bulk_io.pending = false;
            xSemaphoreGive(s_bulk_io_mutex);
            neo_debug_xfer("usb_in", ESP_ERR_TIMEOUT, buffer, *out_length);
            return ESP_ERR_TIMEOUT;
        }
        err = s_bulk_io.result;
        got = s_bulk_io.actual_length;
        xSemaphoreGive(s_bulk_io_mutex);
    }

    if (err != ESP_OK) {
        neo_debug_xfer("usb_in", err, buffer, *out_length);
        return err;
    }
    *out_length = pre + got;
    neo_debug_xfer("usb_in", ESP_OK, buffer, *out_length > 8 ? 8 : *out_length);
    return ESP_OK;
}

esp_err_t usb_host_neo_write(const uint8_t *buffer, size_t length, int timeout_ms)
{
    if (!buffer && length != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_neo.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (timeout_ms <= 0) {
        timeout_ms = 1000;
    }

    esp_err_t err = ESP_OK;
    size_t offset = 0;
    size_t out_mps = s_neo.out_mps > 0 ? s_neo.out_mps : NEO_USB_PROTO_CHUNK;
    size_t out_chunk = out_mps > NEO_USB_PROTO_CHUNK ? NEO_USB_PROTO_CHUNK : out_mps;
    while (offset < length && err == ESP_OK) {
        size_t chunk = length - offset;
        if (chunk > out_chunk) {
            chunk = out_chunk;
        }
        size_t sent = 0;
        err = neo_usb_bulk_transfer(&s_neo, s_neo.out_ep, (uint8_t *)(buffer + offset), chunk, false, &sent,
                                    timeout_ms);
        neo_debug_xfer("usb_out", err, buffer + offset, sent);
        if (sent == 0 && err == ESP_OK) {
            err = ESP_FAIL;
        }
        offset += sent;
    }
    return err;
}

esp_err_t usb_host_neo_get_version(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!usb_host_neo_is_connected()) {
        neo_debug_event("get_version neo not connected");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t comms = usb_host_neo_ensure_comms();
    if (comms != ESP_OK) {
        neo_debug_event("get_version ensure_comms failed: %s", esp_err_to_name(comms));
        return comms;
    }

    neo_device_lock();
    neo_debug_event("VERSION start");
    esp_err_t result = neo_device_dialogue_start(NEO_APPLET_ID_SYSTEM);
    if (result != ESP_OK) {
        neo_debug_event("get_version dialogue_start failed: %s", esp_err_to_name(result));
        neo_device_unlock();
        return result;
    }

    neo_message_t response;
    result = neo_device_query_version_message(&response);
    if (result != ESP_OK) {
        neo_debug_event("VERSION query failed: %s", esp_err_to_name(result));
        neo_device_dialogue_end();
        neo_device_unlock();
        return result;
    }

    size_t payload_size = neo_message_argument(&response, 1, 4);
    if (payload_size == 0 || payload_size > 4096) {
        neo_debug_event("VERSION bad payload size %u", (unsigned)payload_size);
        neo_device_dialogue_end();
        neo_device_unlock();
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *payload = malloc(payload_size);
    if (!payload) {
        neo_debug_event("VERSION malloc failed size=%u", (unsigned)payload_size);
        neo_device_dialogue_end();
        neo_device_unlock();
        return ESP_ERR_NO_MEM;
    }

    size_t received = 0;
    result = neo_device_read_exact(payload, payload_size, (int)(payload_size * 10 + 600), &received);
    neo_device_dialogue_end();
    if (result != ESP_OK || received != payload_size) {
        neo_debug_event("VERSION payload read failed: %s (got %u/%u)", esp_err_to_name(result), (unsigned)received,
                        (unsigned)payload_size);
        free(payload);
        neo_device_unlock();
        return result;
    }

    if (received >= 6) {
        snprintf(out, out_size, "%u.%u %.*s", payload[4], payload[5], 19, (const char *)(payload + 6));
    } else {
        snprintf(out, out_size, "Neo2");
    }
    neo_debug_event("VERSION ok: %s", out);
    free(payload);
    neo_device_unlock();
    return ESP_OK;
}

esp_err_t usb_host_neo_list_applets(char *out_json, size_t out_size)
{
    if (!out_json || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!usb_host_neo_is_connected()) {
        neo_debug_event("list_applets neo not connected");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t comms = usb_host_neo_ensure_comms();
    if (comms != ESP_OK) {
        neo_debug_event("list_applets ensure_comms failed: %s", esp_err_to_name(comms));
        return comms;
    }

    neo_applet_info_t *applets = calloc(32, sizeof(neo_applet_info_t));
    if (!applets) {
        return ESP_ERR_NO_MEM;
    }
    size_t count = 0;
    esp_err_t result = neo_applet_list(applets, 32, &count);
    if (result != ESP_OK) {
        neo_debug_event("list_applets failed: %s", esp_err_to_name(result));
        free(applets);
        return result;
    }
    neo_debug_event("list_applets ok count=%u", (unsigned)count);

    size_t offset = 0;
    int written = snprintf(out_json, out_size, "[");
    if (written < 0 || (size_t)written >= out_size) {
        free(applets);
        return ESP_ERR_INVALID_SIZE;
    }
    offset = (size_t)written;
    for (size_t index = 0; index < count; index++) {
        written = snprintf(out_json + offset, out_size - offset,
                           "%s{\"id\":%u,\"name\":\"%s\",\"rom_size\":%lu,\"ram_size\":%lu,\"file_count\":%u}",
                           index == 0 ? "" : ",", applets[index].applet_id, applets[index].name,
                           (unsigned long)applets[index].rom_size, (unsigned long)applets[index].ram_size,
                           applets[index].file_count);
        if (written < 0 || (size_t)written >= out_size - offset) {
            free(applets);
            return ESP_ERR_INVALID_SIZE;
        }
        offset += (size_t)written;
    }
    free(applets);
    if (offset + 2 > out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    out_json[offset++] = ']';
    out_json[offset] = '\0';
    return ESP_OK;
}

esp_err_t usb_host_neo_list_files(cJSON *files)
{
    if (!files) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!usb_host_neo_is_connected()) {
        neo_debug_event("file_scan neo not connected");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t comms = usb_host_neo_ensure_comms();
    if (comms != ESP_OK) {
        neo_debug_event("file_scan ensure_comms failed: %s", esp_err_to_name(comms));
        return comms;
    }

    neo_applet_info_t *applets = calloc(32, sizeof(neo_applet_info_t));
    if (!applets) {
        return ESP_ERR_NO_MEM;
    }

    size_t applet_count = 0;
    esp_err_t err = neo_applet_list(applets, 32, &applet_count);
    if (err != ESP_OK) {
        free(applets);
        neo_debug_event("file_scan list_applets failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Neo file scan: %u applets on device", (unsigned)applet_count);
    neo_debug_event("file_scan start applets=%u", (unsigned)applet_count);

    for (size_t ai = 0; ai < applet_count; ai++) {
        uint16_t applet_id = applets[ai].applet_id;
        if (applet_id == NEO_APPLET_ID_SYSTEM || applets[ai].file_count == 0) {
            continue;
        }

        cJSON *applet_files = cJSON_CreateArray();
        if (!applet_files) {
            free(applets);
            return ESP_ERR_NO_MEM;
        }
        err = neo_file_list_applet(applet_id, applet_files);
        if (err != ESP_OK) {
            cJSON_Delete(applet_files);
            ESP_LOGW(TAG, "file scan applet=0x%04x failed: %s", applet_id, esp_err_to_name(err));
            neo_debug_event("file_scan fail applet=0x%04x %s", applet_id, esp_err_to_name(err));
            continue;
        }
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, applet_files)
        {
            cJSON *copy = cJSON_Duplicate(item, 1);
            if (!copy) {
                cJSON_Delete(applet_files);
                free(applets);
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddStringToObject(copy, "applet_name", applets[ai].name);
            cJSON_AddItemToArray(files, copy);
        }
        cJSON_Delete(applet_files);
        esp_task_wdt_reset();
        vTaskDelay(1);
    }

    free(applets);
    neo_debug_event("file_scan done files=%d", cJSON_GetArraySize(files));
    ESP_LOGI(TAG, "Neo file scan complete: %d files", cJSON_GetArraySize(files));
    return ESP_OK;
}

esp_err_t usb_host_neo_read_raw_file(uint8_t file_index, uint8_t *buffer, size_t buffer_size, size_t *out_len)
{
    return usb_host_neo_read_file(NEO_APPLET_ID_ALPHAWORD, file_index, buffer, buffer_size, out_len);
}

esp_err_t usb_host_neo_read_file_with_size(uint16_t applet_id, uint8_t file_index, uint32_t alloc_size,
                                           uint8_t *buffer, size_t buffer_size, size_t *out_len)
{
    if (!buffer || buffer_size == 0 || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!usb_host_neo_is_connected()) {
        neo_debug_event("read_file neo not connected");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t comms = usb_host_neo_ensure_comms();
    if (comms != ESP_OK) {
        neo_debug_event("read_file ensure_comms failed: %s", esp_err_to_name(comms));
        return comms;
    }

    neo_debug_event("READ_FILE start applet=0x%04x index=%u alloc=%lu", applet_id, file_index,
                    (unsigned long)alloc_size);
    esp_err_t err =
        neo_file_read_raw_with_size(applet_id, file_index, alloc_size, buffer, buffer_size, out_len);
    if (err == ESP_OK) {
        neo_debug_event("READ_FILE ok applet=0x%04x index=%u bytes=%u", applet_id, file_index, (unsigned)*out_len);
    } else {
        neo_debug_event("READ_FILE failed applet=0x%04x index=%u: %s", applet_id, file_index, esp_err_to_name(err));
    }
    return err;
}

esp_err_t usb_host_neo_read_file_alloc(uint16_t applet_id, uint8_t file_index, neo_file_attr_t *attrs_out,
                                       uint8_t **out_data, size_t *out_len, size_t max_bytes)
{
    if (!out_data || !out_len || max_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_data = NULL;
    *out_len = 0;
    if (!usb_host_neo_is_connected()) {
        neo_debug_event("read_file_alloc neo not connected");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t comms = usb_host_neo_ensure_comms();
    if (comms != ESP_OK) {
        neo_debug_event("read_file_alloc ensure_comms failed: %s", esp_err_to_name(comms));
        return comms;
    }

    neo_debug_event("READ_FILE_ALLOC start applet=0x%04x index=%u", applet_id, file_index);
    esp_err_t err = neo_file_read_alloc(applet_id, file_index, attrs_out, out_data, out_len, max_bytes);
    if (err == ESP_OK) {
        neo_debug_event("READ_FILE_ALLOC ok applet=0x%04x index=%u bytes=%u", applet_id, file_index,
                        (unsigned)*out_len);
    } else {
        neo_debug_event("READ_FILE_ALLOC failed applet=0x%04x index=%u: %s", applet_id, file_index,
                        esp_err_to_name(err));
    }
    return err;
}

esp_err_t usb_host_neo_read_file(uint16_t applet_id, uint8_t file_index, uint8_t *buffer, size_t buffer_size,
                                 size_t *out_len)
{
    if (!buffer || buffer_size == 0 || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!usb_host_neo_is_connected()) {
        neo_debug_event("read_file neo not connected");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t comms = usb_host_neo_ensure_comms();
    if (comms != ESP_OK) {
        neo_debug_event("read_file ensure_comms failed: %s", esp_err_to_name(comms));
        return comms;
    }

    neo_debug_event("READ_FILE start applet=0x%04x index=%u", applet_id, file_index);
    esp_err_t err = neo_file_read_raw(applet_id, file_index, buffer, buffer_size, out_len);
    if (err == ESP_OK) {
        neo_debug_event("READ_FILE ok applet=0x%04x index=%u bytes=%u", applet_id, file_index, (unsigned)*out_len);
    } else {
        neo_debug_event("READ_FILE failed applet=0x%04x index=%u: %s", applet_id, file_index, esp_err_to_name(err));
    }
    return err;
}

esp_err_t usb_host_neo_install_applet(const uint8_t *content, size_t length, bool replace_existing)
{
    if (!content || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t comms = usb_host_neo_ensure_comms();
    if (comms != ESP_OK) {
        return comms;
    }
    return neo_applet_install(content, length, replace_existing);
}

esp_err_t usb_host_neo_remove_applet(uint16_t applet_id)
{
    esp_err_t comms = usb_host_neo_ensure_comms();
    if (comms != ESP_OK) {
        return comms;
    }
    return neo_applet_remove(applet_id);
}

esp_err_t usb_host_neo_fetch_applet(uint16_t applet_id, uint8_t *buffer, size_t capacity, size_t *out_length)
{
    esp_err_t comms = usb_host_neo_ensure_comms();
    if (comms != ESP_OK) {
        return comms;
    }
    return neo_applet_fetch(applet_id, buffer, capacity, out_length);
}

esp_err_t usb_host_neo_write_file_raw(uint16_t applet_id, uint8_t file_index, const uint8_t *data, size_t length)
{
    esp_err_t comms = usb_host_neo_ensure_comms();
    if (comms != ESP_OK) {
        return comms;
    }
    return neo_file_write_raw(applet_id, file_index, data, length);
}

esp_err_t usb_host_neo_clear_file(uint16_t applet_id, uint8_t file_index)
{
    esp_err_t comms = usb_host_neo_ensure_comms();
    if (comms != ESP_OK) {
        return comms;
    }
    return neo_file_clear(applet_id, file_index);
}

esp_err_t usb_host_neo_get_file_attributes(uint8_t file_index, char *out_json, size_t out_size)
{
    if (!out_json || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!usb_host_neo_is_connected()) {
        neo_debug_event("get_file_attributes neo not connected");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t comms = usb_host_neo_ensure_comms();
    if (comms != ESP_OK) {
        neo_debug_event("get_file_attributes ensure_comms failed: %s", esp_err_to_name(comms));
        return comms;
    }

    neo_file_attr_t attrs;
    esp_err_t err = neo_get_file_attributes(NEO_APPLET_ID_ALPHAWORD, file_index, &attrs);
    if (err == ESP_ERR_NOT_FOUND) {
        snprintf(out_json, out_size, "{\"error\":\"not_found\"}");
        neo_debug_event("file_attrs index=%u not found", file_index);
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        neo_debug_event("file_attrs index=%u failed: %s", file_index, esp_err_to_name(err));
        return err;
    }

    neo_debug_event("file_attrs index=%u name=%s size=%lu", file_index, attrs.name,
                    (unsigned long)attrs.alloc_size);

    snprintf(out_json, out_size,
             "{\"index\":%u,\"name\":\"%s\",\"alloc_size\":%lu,\"min_size\":%lu,\"flags\":%u,\"file_space\":%u}",
             file_index, attrs.name, (unsigned long)attrs.alloc_size, (unsigned long)attrs.min_size,
             (unsigned)attrs.flags,
             attrs.file_space);
    return ESP_OK;
}

esp_err_t usb_host_neo_get_last_device_info(neo_usb_dev_info_t *info)
{
    if (!info) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_neo.last_dev.vendor_id == 0) {
        return ESP_FAIL;
    }
    *info = s_neo.last_dev;
    return ESP_OK;
}

/**
 * Return Neo to keyboard emulation (NeoTools RESTART). Call after backups so
 * the device is usable as a typewriter again — portal "Backup all" and
 * autobackup both rely on this.
 */
esp_err_t usb_host_neo_restart(void)
{
    if (!usb_host_neo_is_connected() && !usb_host_neo_is_comms_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t comms = usb_host_neo_ensure_comms();
    if (comms != ESP_OK) {
        return comms;
    }
    esp_err_t err = neo_device_restart();
    if (err != ESP_OK) {
        return err;
    }
    s_neo.ready = false;
    s_neo.in_ep = 0;
    s_neo.out_ep = 0;
    for (int i = 0; i < 80 && !s_neo.keyboard_active; i++) {
        neo_usb_pump_ms(&s_neo, 100);
    }
    if (!s_neo.keyboard_active) {
        neo_debug_event("restart ok but keyboard mode not seen yet");
    }
    return ESP_OK;
}

esp_err_t usb_host_neo_remove_all_applets(void)
{
    esp_err_t comms = usb_host_neo_ensure_comms();
    if (comms != ESP_OK) {
        return comms;
    }
    return neo_applet_remove_all();
}

esp_err_t usb_host_neo_write_file_by_name(uint16_t applet_id, const char *name_or_space, const char *password,
                                          const uint8_t *data, size_t length)
{
    esp_err_t comms = usb_host_neo_ensure_comms();
    if (comms != ESP_OK) {
        return comms;
    }
    return neo_file_write_or_create(applet_id, name_or_space, password, data, length);
}

esp_err_t usb_host_neo_clear_file_by_name(uint16_t applet_id, const char *name_or_space)
{
    esp_err_t comms = usb_host_neo_ensure_comms();
    if (comms != ESP_OK) {
        return comms;
    }
    return neo_file_clear_by_name_or_space(applet_id, name_or_space);
}

esp_err_t usb_host_neo_inspect_applet(const uint8_t *content, size_t length, char *out_json, size_t out_size)
{
    if (!content || length == 0 || !out_json || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    neo_applet_info_t info;
    esp_err_t err = neo_applet_inspect(content, length, &info);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(out_json, out_size,
             "{\"applet_id\":%u,\"name\":\"%.36s\",\"rom_size\":%lu,\"ram_size\":%lu,\"file_count\":%u,"
             "\"version\":\"%u.%u.%u\",\"file_space\":%lu}",
             info.applet_id, info.name, (unsigned long)info.rom_size, (unsigned long)info.ram_size, info.file_count,
             info.version_major, info.version_minor, info.version_revision, (unsigned long)info.file_space);
    return ESP_OK;
}

esp_err_t usb_host_neo_backup_all_files(uint16_t applet_id, neo_charmap_id_t map, cJSON *out_saved)
{
    if (!out_saved) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t comms = usb_host_neo_ensure_comms();
    if (comms != ESP_OK) {
        return comms;
    }

    /* Keep this off the caller's stack — each saved_file_t is ~312 bytes,
     * so 32 entries alone (~10KB) overflow the UART console_repl task. */
    enum { kMaxResults = 32 };
    neo_import_saved_file_t *results = calloc(kMaxResults, sizeof(*results));
    if (!results) {
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0;
    esp_err_t err = neo_import_backup_applet_files(applet_id, map, results, kMaxResults, &count);
    if (err != ESP_OK) {
        free(results);
        return err;
    }
    for (size_t i = 0; i < count; ++i) {
        cJSON *entry = cJSON_CreateObject();
        if (!entry) {
            free(results);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(entry, "name", results[i].name);
        cJSON_AddStringToObject(entry, "path", results[i].path);
        cJSON_AddNumberToObject(entry, "file_index", results[i].file_index);
        cJSON_AddNumberToObject(entry, "bytes", (double)results[i].bytes_saved);
        cJSON_AddItemToArray(out_saved, entry);
    }
    free(results);
    return ESP_OK;
}

const char *usb_host_neo_get_mode(void)
{
    neo_usb_host_status_t status;
    usb_host_neo_get_host_status(&status);
    if (status.neo_ready) {
        return "comms";
    }
    if (status.keyboard_active) {
        return "keyboard";
    }
    neo_usb_dev_info_t dev;
    if (usb_host_neo_get_last_device_info(&dev) == ESP_OK) {
        if (dev.product_id == NEO_USB_PID_HID) {
            return "keyboard";
        }
        if (dev.product_id == NEO_USB_PID_COM) {
            return "comms";
        }
    }
    return "unknown";
}

esp_err_t usb_host_neo_get_system_info(cJSON *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t comms = usb_host_neo_ensure_comms();
    if (comms != ESP_OK) {
        return comms;
    }

    char version[64];
    esp_err_t err = usb_host_neo_get_version(version, sizeof(version));
    if (err != ESP_OK) {
        return err;
    }
    cJSON_AddStringToObject(out, "version", version);
    cJSON_AddStringToObject(out, "mode", usb_host_neo_get_mode());

    neo_avail_space_t space = {0};
    err = neo_space_get_available(&space);
    if (err != ESP_OK) {
        return err;
    }
    cJSON_AddNumberToObject(out, "free_rom", (double)space.free_rom);
    cJSON_AddNumberToObject(out, "free_ram", (double)space.free_ram);
    return ESP_OK;
}
