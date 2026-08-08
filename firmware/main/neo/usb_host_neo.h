/**
 * @file usb_host_neo.h
 * @brief USB transport: keyboard mode (0xBD04) ↔ comms mode (0xBD01).
 *
 * USB LAYER (below neo_device)
 * ==============================
 * The Neo2 exposes ONE USB device that changes personality:
 *
 *   PID 0xBD04 — HID keyboard. User can type; we may listen to HID reports
 *                (neo_usb_hid.c) but do NOT run ASM protocol in this mode.
 *   PID 0xBD01 — Bulk IN/OUT endpoints. ASM hello + 8-byte messages live here.
 *   PID 0x0100 — Rare hub stage during re-enumeration; wait, do not treat as ready.
 *
 * FLIP SEQUENCE (NeoTools, not guessed)
 * -------------------------------------
 * While still 0xBD04:
 *   SET_CONFIGURATION(1)
 *   Five vendor control transfers with payload bytes 0xE0,0xE1,0xE2,0xE3,0xE4
 * Neo disconnects and reappears as 0xBD01. We wait ~4s and may retry once.
 *
 * CALLERS SHOULD USE
 * ------------------
 *   usb_host_neo_ensure_comms()  — flip if needed, return when bulk I/O works
 *   usb_host_neo_read/write()    — raw bytes (neo_device loops 8-byte reads internally)
 *   usb_host_neo_restart()       — REQUEST_RESTART then wait for 0xBD04 again
 *
 * Implementation detail: all USB transfers run on the dedicated client task;
 * other tasks request flips via NEO_USB_ACTION_FLIP. See usb_host_neo.c.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"
#include "neo_conv.h"
#include "neo_file.h"

esp_err_t usb_host_neo_init(void);
bool usb_host_neo_is_connected(void);
esp_err_t usb_host_neo_read(uint8_t *buffer, size_t capacity, int timeout_ms, size_t *out_length);
esp_err_t usb_host_neo_write(const uint8_t *buffer, size_t length, int timeout_ms);
void usb_host_neo_publish_keyboard_text(const char *text, size_t length);

typedef struct {
    uint16_t vendor_id;
    uint16_t product_id;
    char product_string[64];
} neo_usb_dev_info_t;

esp_err_t usb_host_neo_get_last_device_info(neo_usb_dev_info_t *info);

typedef struct {
    bool host_installed;
    bool neo_ready;          /**< Bulk comms session open (0xBD01). */
    bool keyboard_active;    /**< HID session (0xBD04). */
    bool flipping;           /**< Flip in progress; not ready for protocol yet. */
    int bus_device_count;
    int flip_attempt;
    uint16_t last_vid;
    uint16_t last_pid;
} neo_usb_host_status_t;

void usb_host_neo_get_host_status(neo_usb_host_status_t *out);

typedef struct {
    int bus_device_count;
    bool neo_ready;
    bool flipping;
    int devices_found;
    uint16_t vid[4];
    uint16_t pid[4];
} neo_usb_scan_result_t;

/** Scan OTG1, log devices, start flip if VID 0x081E seen. */
esp_err_t usb_host_neo_rescan(neo_usb_scan_result_t *out);

bool usb_host_neo_is_comms_ready(void);

/** Block until comms bulk endpoints are usable (may flip from keyboard). */
esp_err_t usb_host_neo_ensure_comms(void);

/** Clear RX staging before neo_device_dialogue_start hello (brief drain, no fixed sleep). */
void usb_host_neo_prepare_dialogue(void);
void usb_host_neo_recover_transport(void);
void usb_host_neo_invalidate_comms(void);

esp_err_t usb_host_neo_get_version(char *out, size_t out_size);
esp_err_t usb_host_neo_list_applets(char *out_json, size_t out_size);
esp_err_t usb_host_neo_list_files(cJSON *files);
esp_err_t usb_host_neo_get_file_attributes(uint8_t file_index, char *out_json, size_t out_size);
esp_err_t usb_host_neo_read_raw_file(uint8_t file_index, uint8_t *buffer, size_t buffer_size, size_t *out_len);
esp_err_t usb_host_neo_read_file(uint16_t applet_id, uint8_t file_index, uint8_t *buffer, size_t buffer_size,
                                 size_t *out_len);
esp_err_t usb_host_neo_read_file_with_size(uint16_t applet_id, uint8_t file_index, uint32_t alloc_size,
                                           uint8_t *buffer, size_t buffer_size, size_t *out_len);
/**
 * Attributes + payload in one ASM dialogue. Caller frees *out_data when non-NULL.
 * @param attrs_out optional metadata; @param max_bytes allocation cap (e.g. 256*1024).
 */
esp_err_t usb_host_neo_read_file_alloc(uint16_t applet_id, uint8_t file_index, neo_file_attr_t *attrs_out,
                                       uint8_t **out_data, size_t *out_len, size_t max_bytes);
esp_err_t usb_host_neo_install_applet(const uint8_t *content, size_t length, bool replace_existing);
esp_err_t usb_host_neo_remove_applet(uint16_t applet_id);
esp_err_t usb_host_neo_fetch_applet(uint16_t applet_id, uint8_t *buffer, size_t capacity, size_t *out_length);
esp_err_t usb_host_neo_write_file_raw(uint16_t applet_id, uint8_t file_index, const uint8_t *data, size_t length);
esp_err_t usb_host_neo_clear_file(uint16_t applet_id, uint8_t file_index);
esp_err_t usb_host_neo_restart(void);
esp_err_t usb_host_neo_remove_all_applets(void);
esp_err_t usb_host_neo_write_file_by_name(uint16_t applet_id, const char *name_or_space, const char *password,
                                          const uint8_t *data, size_t length);
esp_err_t usb_host_neo_clear_file_by_name(uint16_t applet_id, const char *name_or_space);
esp_err_t usb_host_neo_inspect_applet(const uint8_t *content, size_t length, char *out_json, size_t out_size);
esp_err_t usb_host_neo_backup_all_files(uint16_t applet_id, neo_charmap_id_t map, cJSON *out_saved);
const char *usb_host_neo_get_mode(void);
esp_err_t usb_host_neo_get_system_info(cJSON *out);
