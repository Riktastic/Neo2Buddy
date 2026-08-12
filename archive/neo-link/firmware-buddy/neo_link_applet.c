/**

 * @file neo_link_applet.c

 * @brief Bundled NeoLinkChat install, version check, and auto-sync on Neo connect.

 */



#include "neo_link_applet.h"



#include "log_buffer.h"

#include "neo_autobackup.h"

#include "neo_link_limits.h"

#include "neo_file.h"

#include "neo_link_mailbox.h"

#include "usb_host_neo.h"



#include "esp_log.h"

#include "freertos/FreeRTOS.h"

#include "freertos/semphr.h"

#include "freertos/task.h"



#include <stdio.h>
#include <stdlib.h>
#include <string.h>



static const char *TAG = "neo_link_applet";



#define NEO_LINK_APPLET_SYNC_DELAY_MS 4000

#define NEO_LINK_APPLET_SYNC_STACK 8192



extern const uint8_t _binary_NeoLinkChat_OS3KApp_start[];

extern const uint8_t _binary_NeoLinkChat_OS3KApp_end[];



static SemaphoreHandle_t s_lock;

static TaskHandle_t s_sync_task;

static volatile bool s_sync_busy;

static neo_link_applet_status_t s_status;



static uint32_t neo_link_applet_version_key(uint8_t major, uint8_t minor, uint8_t rev)

{

    return ((uint32_t)major << 16) | ((uint32_t)minor << 8) | (uint32_t)rev;

}



static void neo_link_applet_refresh_bundled(void)

{

    neo_applet_info_t bundled;



    if (neo_link_applet_bundled_info(&bundled) != ESP_OK) {

        return;

    }

    s_status.bundled_major = bundled.version_major;

    s_status.bundled_minor = bundled.version_minor;

    s_status.bundled_rev = bundled.version_revision;

    s_status.bundled_ram = bundled.ram_size;

    s_status.bundled_rom = bundled.rom_size;

}



const uint8_t *neo_link_applet_blob(size_t *out_len)

{

    size_t len = (size_t)(_binary_NeoLinkChat_OS3KApp_end - _binary_NeoLinkChat_OS3KApp_start);

    if (out_len) {

        *out_len = len;

    }

    return _binary_NeoLinkChat_OS3KApp_start;

}



esp_err_t neo_link_applet_bundled_info(neo_applet_info_t *out)

{

    size_t len = 0;

    const uint8_t *blob = neo_link_applet_blob(&len);

    if (!blob || len == 0 || !out) {

        return ESP_ERR_INVALID_STATE;

    }

    return neo_applet_inspect(blob, len, out);

}



bool neo_link_applet_installed_is_current(const neo_applet_info_t *installed)

{

    neo_applet_info_t bundled;



    if (!installed) {

        return false;

    }

    if (installed->applet_id != NEO_LINK_APPLET_ID) {

        return false;

    }

    if (neo_link_applet_bundled_info(&bundled) != ESP_OK) {

        return false;

    }

    if (installed->ram_size > NEO_LINK_APPLET_RAM_LEGACY_MAX) {

        return false;

    }

    if (installed->ram_size > bundled.ram_size + 64) {

        return false;

    }

    if (installed->rom_size != bundled.rom_size) {

        return false;

    }

    return neo_link_applet_version_key(installed->version_major, installed->version_minor,

                                       installed->version_revision) >=

           neo_link_applet_version_key(bundled.version_major, bundled.version_minor,

                                       bundled.version_revision);

}



static esp_err_t neo_link_applet_find_installed(neo_applet_info_t *out, bool *found)

{

    neo_applet_info_t list[32];

    size_t count = 0;



    if (!out || !found) {

        return ESP_ERR_INVALID_ARG;

    }

    *found = false;

    memset(out, 0, sizeof(*out));



    esp_err_t err = neo_applet_list(list, 32, &count);

    if (err != ESP_OK) {

        return err;

    }

    for (size_t i = 0; i < count; i++) {

        if (list[i].applet_id == NEO_LINK_APPLET_ID) {

            *out = list[i];

            *found = true;

            return ESP_OK;

        }

    }

    return ESP_OK;

}



void neo_link_applet_init(void)

{

    if (s_lock == NULL) {

        s_lock = xSemaphoreCreateMutex();

    }

    neo_link_applet_refresh_bundled();

}



void neo_link_applet_get_status(neo_link_applet_status_t *out)

{

    if (!out) {

        return;

    }

    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {

        *out = s_status;

        out->sync_busy = s_sync_busy;

        xSemaphoreGive(s_lock);

        return;

    }

    *out = s_status;

    out->sync_busy = s_sync_busy;

}



#if NEO_LINK_APPLET_MAILBOXES_ENABLED

/** Create NeoLinkIn/NeoLinkOut if the applet header reserves fileCount=2 but slots are empty. */

static esp_err_t neo_link_applet_seed_mailbox_files(void)

{

    static const uint8_t seed = ' ';

    neo_file_attr_t attrs;

    esp_err_t err;



    err = neo_file_find_by_name_or_space(NEO_LINK_APPLET_ID, NEO_LINK_MAILBOX_IN_NAME, &attrs);

    if (err == ESP_ERR_NOT_FOUND) {

        err = neo_file_create(NEO_LINK_APPLET_ID, NEO_LINK_MAILBOX_IN_NAME, "write", &seed, 1);

        if (err != ESP_OK) {

            ESP_LOGW(TAG, "NeoLinkIn create failed: %s", esp_err_to_name(err));

            return err;

        }

    } else if (err != ESP_OK) {

        return err;

    }



    err = neo_file_find_by_name_or_space(NEO_LINK_APPLET_ID, NEO_LINK_MAILBOX_OUT_NAME, &attrs);

    if (err == ESP_ERR_NOT_FOUND) {

        err = neo_file_create(NEO_LINK_APPLET_ID, NEO_LINK_MAILBOX_OUT_NAME, "write", &seed, 1);

        if (err != ESP_OK) {

            ESP_LOGW(TAG, "NeoLinkOut create failed: %s", esp_err_to_name(err));

        }

        return err;

    }

    return err;

}

#endif



static esp_err_t neo_link_applet_verify_installed(const uint8_t *blob, size_t len)

{

    /* Neo often reports romUsage rounded up (562 → 564). Leave headroom for READ_APPLET. */

    const size_t capacity = len + 64;

    uint8_t *readback = NULL;

    size_t got = 0;

    esp_err_t err;



    if (!blob || len == 0) {

        return ESP_ERR_INVALID_ARG;

    }

    readback = (uint8_t *)malloc(capacity);

    if (!readback) {

        return ESP_ERR_NO_MEM;

    }

    err = usb_host_neo_fetch_applet(NEO_LINK_APPLET_ID, readback, capacity, &got);

    if (err != ESP_OK) {

        ESP_LOGW(TAG, "install verify: fetch failed: %s", esp_err_to_name(err));

        free(readback);

        return err;

    }

    if (got < len) {

        ESP_LOGW(TAG, "install verify: Neo returned only %u bytes (expected >= %u)", (unsigned)got,

                 (unsigned)len);

        free(readback);

        return ESP_ERR_INVALID_SIZE;

    }

    if (got != len) {

        ESP_LOGI(TAG, "install verify: Neo rom padded %u → %u (comparing first %u bytes)", (unsigned)len,

                 (unsigned)got, (unsigned)len);

    }

    if (memcmp(readback, blob, len) != 0) {

        for (size_t i = 0; i < len; i++) {

            if (readback[i] != blob[i]) {

                ESP_LOGW(TAG, "install verify: byte mismatch at %u (neo=0x%02x bundled=0x%02x)",

                         (unsigned)i, readback[i], blob[i]);

                break;

            }

        }

        free(readback);

        return ESP_ERR_INVALID_CRC;

    }

    ESP_LOGI(TAG, "install verify: Neo blob matches bundled (%u bytes)", (unsigned)len);

    free(readback);

    return ESP_OK;

}



esp_err_t neo_link_applet_install(bool replace_existing)

{

    size_t len = 0;

    const uint8_t *blob = neo_link_applet_blob(&len);

    if (!blob || len == 0) {

        return ESP_ERR_NOT_FOUND;

    }

    ESP_LOGI(TAG, "Installing NeoLinkChat (%u bytes, replace=%d)", (unsigned)len,

             replace_existing ? 1 : 0);

    esp_err_t err = usb_host_neo_install_applet(blob, len, replace_existing);

    if (err == ESP_OK) {

        esp_err_t verify = neo_link_applet_verify_installed(blob, len);

        if (verify != ESP_OK) {

            /* Install already committed; Neo often reboots before READ_APPLET finishes. */

            ESP_LOGW(TAG, "install verify skipped/failed (%s) — WRITE_APPLET already OK",

                     esp_err_to_name(verify));

            log_buffer_appendf("neo_link: install OK, verify warn: %s", esp_err_to_name(verify));

        } else {

            log_buffer_appendf("neo_link: installed NeoLinkChat applet 0x%04X (%u bytes)",

                               NEO_LINK_APPLET_ID, (unsigned)len);

        }

#if NEO_LINK_APPLET_MAILBOXES_ENABLED
        esp_err_t files = neo_link_applet_seed_mailbox_files();

        if (files != ESP_OK) {

            ESP_LOGW(TAG, "mailbox seed after install: %s", esp_err_to_name(files));

        }
#endif

    } else {

        log_buffer_appendf("neo_link: install NeoLinkChat failed: %s", esp_err_to_name(err));

    }

    return err;

}



esp_err_t neo_link_applet_ensure_current(bool force)

{

    neo_applet_info_t installed;

    bool found = false;

    esp_err_t err;



    if (!usb_host_neo_is_connected()) {

        return ESP_ERR_INVALID_STATE;

    }



    neo_link_applet_refresh_bundled();



    err = usb_host_neo_ensure_comms();

    if (err != ESP_OK) {

        strlcpy(s_status.last_sync_msg, "comms flip failed", sizeof(s_status.last_sync_msg));

        s_status.last_sync_err = err;

        return err;

    }



    err = neo_link_applet_find_installed(&installed, &found);

    if (err != ESP_OK) {

        (void)usb_host_neo_restart();

        strlcpy(s_status.last_sync_msg, "list applets failed", sizeof(s_status.last_sync_msg));

        s_status.last_sync_err = err;

        return err;

    }



    s_status.installed = found;

    if (found) {

        s_status.installed_major = installed.version_major;

        s_status.installed_minor = installed.version_minor;

        s_status.installed_rev = installed.version_revision;

        s_status.installed_ram = installed.ram_size;

        s_status.installed_rom = installed.rom_size;

    }



    if (!force && found && neo_link_applet_installed_is_current(&installed)) {

        s_status.up_to_date = true;

        s_status.last_sync_err = ESP_OK;

        snprintf(s_status.last_sync_msg, sizeof(s_status.last_sync_msg), "applet %u.%u.%c ok",

                 (unsigned)installed.version_major, (unsigned)installed.version_minor,

                 (char)installed.version_revision);

        ESP_LOGI(TAG, "NeoLinkChat already current (%s, ram=%lu)", s_status.last_sync_msg,

                 (unsigned long)installed.ram_size);

#if NEO_LINK_APPLET_MAILBOXES_ENABLED
        (void)neo_link_applet_seed_mailbox_files();
#endif

        (void)usb_host_neo_restart();

        return ESP_OK;

    }



    if (!found) {

        ESP_LOGI(TAG, "NeoLinkChat missing on Neo — installing bundled v%u.%u.%c",

                 (unsigned)s_status.bundled_major, (unsigned)s_status.bundled_minor,

                 (char)s_status.bundled_rev);

        strlcpy(s_status.last_sync_msg, "installing (missing)", sizeof(s_status.last_sync_msg));

    } else {

        ESP_LOGW(TAG,

                 "NeoLinkChat outdated on Neo (installed %u.%u.%c rom=%lu ram=%lu) — replacing with "

                 "bundled %u.%u.%c rom=%lu ram=%lu",

                 (unsigned)installed.version_major, (unsigned)installed.version_minor,

                 (char)installed.version_revision, (unsigned long)installed.rom_size,

                 (unsigned long)installed.ram_size,

                 (unsigned)s_status.bundled_major, (unsigned)s_status.bundled_minor,

                 (char)s_status.bundled_rev, (unsigned long)s_status.bundled_rom,

                 (unsigned long)s_status.bundled_ram);

        snprintf(s_status.last_sync_msg, sizeof(s_status.last_sync_msg),

                 "replacing %u.%u.%c ram=%lu", (unsigned)installed.version_major,

                 (unsigned)installed.version_minor, (char)installed.version_revision,

                 (unsigned long)installed.ram_size);

    }



    err = neo_link_applet_install(true);

    (void)usb_host_neo_restart();



    if (err == ESP_OK) {

        s_status.installed = true;

        s_status.installed_major = s_status.bundled_major;

        s_status.installed_minor = s_status.bundled_minor;

        s_status.installed_rev = s_status.bundled_rev;

        s_status.installed_ram = s_status.bundled_ram;

        s_status.installed_rom = s_status.bundled_rom;

        s_status.up_to_date = true;

        snprintf(s_status.last_sync_msg, sizeof(s_status.last_sync_msg), "installed %u.%u.%c",

                 (unsigned)s_status.bundled_major, (unsigned)s_status.bundled_minor,

                 (char)s_status.bundled_rev);

        log_buffer_appendf("neo_link: auto-sync %s", s_status.last_sync_msg);

    } else {

        s_status.up_to_date = false;

        snprintf(s_status.last_sync_msg, sizeof(s_status.last_sync_msg), "install failed: %s",

                 esp_err_to_name(err));

    }

    s_status.last_sync_err = err;

    return err;

}



#if CONFIG_BUDDY_NEO_LINK_AUTO_INSTALL_APPLET

static void neo_link_applet_sync_task(void *arg)

{

    (void)arg;



    vTaskDelay(pdMS_TO_TICKS(NEO_LINK_APPLET_SYNC_DELAY_MS));



    for (int i = 0; i < 120 && neo_autobackup_is_busy(); i++) {

        vTaskDelay(pdMS_TO_TICKS(2000));

    }



    if (!usb_host_neo_is_connected()) {

        ESP_LOGI(TAG, "Applet sync skipped (Neo disconnected)");

        goto done;

    }



    (void)neo_link_applet_ensure_current(false);



done:

    if (s_lock && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {

        s_sync_busy = false;

        s_sync_task = NULL;

        xSemaphoreGive(s_lock);

    }

    vTaskDelete(NULL);

}



static esp_err_t neo_link_applet_schedule_sync(void)

{

    if (s_lock == NULL) {

        neo_link_applet_init();

    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {

        return ESP_ERR_INVALID_STATE;

    }

    if (s_sync_busy) {

        xSemaphoreGive(s_lock);

        return ESP_ERR_INVALID_STATE;

    }

    s_sync_busy = true;

    xSemaphoreGive(s_lock);



    BaseType_t ok = xTaskCreate(neo_link_applet_sync_task, "neo_link_app", NEO_LINK_APPLET_SYNC_STACK,

                                NULL, 4, &s_sync_task);

    if (ok != pdPASS) {

        s_sync_busy = false;

        s_sync_task = NULL;

        return ESP_ERR_NO_MEM;

    }

    return ESP_OK;

}

#endif /* CONFIG_BUDDY_NEO_LINK_AUTO_INSTALL_APPLET */



void neo_link_applet_on_neo_connected(void)

{

#if CONFIG_BUDDY_NEO_LINK_AUTO_INSTALL_APPLET

    /* Only when Neo is already in ASM/comms — never flip from keyboard mode here. */

    if (!usb_host_neo_is_comms_ready()) {

        return;

    }

    if (s_status.up_to_date) {

        return;

    }

    if (s_sync_busy) {

        return;

    }

    ESP_LOGI(TAG, "NeoLinkChat auto-check (comms mode)");

    (void)neo_link_applet_schedule_sync();

#endif

}


