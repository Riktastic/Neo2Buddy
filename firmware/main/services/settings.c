/**
 * @file settings.c
 * @brief Persistent device settings using NVS.
 *
 * This module exposes a small, versioned settings block persisted into NVS.
 * It provides helpers to load defaults, read from NVS and write changes back.
 *
 * The implementation uses a simple version check so future schema changes can
 * be detected and migrated if necessary.
 */

#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "settings.h"

#define SETTINGS_NAMESPACE "device"
#define SETTINGS_VERSION 6
static const char *const KEY_AUTO_BACKUP = "auto_bak";
static const char *const KEY_AUTO_CLOUD = "auto_cloud";
static const char *const KEY_NEO_LABEL = "neo_label";

static const char *const KEY_VERSION = "version";
static const char *const KEY_ONBOARDED = "onboarded";
static const char *const KEY_NETWORK_MODE = "net_mode";
static const char *const KEY_HOTSPOT_SSID = "ap_ssid";
static const char *const KEY_HOTSPOT_PASSWORD = "ap_pwd";
static const char *const KEY_BRIGHTNESS = "brightness";
static const char *const KEY_SLEEP_SECONDS = "sleep_secs";
static const char *const KEY_DEVICE_NAME = "name";
static const char *const KEY_KEYBOARD_LAYOUT = "kb_layout";
static const char *const KEY_WIFI_SSID = "wifi_ssid";
static const char *const KEY_WIFI_PASSWORD = "wifi_pwd";
static const char *const KEY_WIFI_DHCP = "wifi_dhcp";
static const char *const KEY_WIFI_IP = "wifi_ip";
static const char *const KEY_WIFI_NETMASK = "wifi_net";
static const char *const KEY_WIFI_GATEWAY = "wifi_gw";
static const char *const KEY_WIFI_DNS = "wifi_dns";

/** Populate a `device_settings_t` with safe defaults. */
void settings_defaults(device_settings_t *settings)
{
    memset(settings, 0, sizeof(*settings));
    settings->display_brightness = 80;
    settings->sleep_timeout_seconds = 300;
    strlcpy(settings->device_name, "Neo2 Buddy", sizeof(settings->device_name));
    /* Default keyboard layout 'u' indicates the user's preferred mapping.
     * See services/ble_hid_device.c for mapping tables. */
    strlcpy(settings->keyboard_layout, "u", sizeof(settings->keyboard_layout));
    settings->wifi_ssid[0] = '\0';
    settings->wifi_password[0] = '\0';
    settings->wifi_dhcp = true;
    settings->wifi_ip[0] = '\0';
    settings->wifi_netmask[0] = '\0';
    settings->wifi_gateway[0] = '\0';
    settings->wifi_dns[0] = '\0';
    settings->require_portal_auth = false;
    settings->auto_backup_on_connect = false;
    settings->auto_cloud_sync_after_backup = false;
    settings->neo_label[0] = '\0';
    settings->network_mode = SETTINGS_NETWORK_DIRECT;
    settings->hotspot_ssid[0] = '\0';
    strlcpy(settings->hotspot_password, "neo2buddy", sizeof(settings->hotspot_password));
}

void settings_apply_hotspot_defaults(device_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }
    if (settings->hotspot_ssid[0] != '\0') {
        return;
    }
    const char *name = settings->device_name[0] ? settings->device_name : "Neo2 Buddy";
    if (strncmp(name, "Neo2", 4) == 0) {
        strlcpy(settings->hotspot_ssid, name, sizeof(settings->hotspot_ssid));
    } else {
        snprintf(settings->hotspot_ssid, sizeof(settings->hotspot_ssid), "Neo2-%.*s",
                 SETTINGS_WIFI_SSID_MAX_LENGTH - 5, name);
    }
    if (settings->hotspot_password[0] == '\0') {
        strlcpy(settings->hotspot_password, "neo2buddy", sizeof(settings->hotspot_password));
    }
}

static void settings_read_wifi_fields(nvs_handle_t handle, device_settings_t *settings)
{
    size_t ssid_len = sizeof(settings->wifi_ssid);
    nvs_get_str(handle, KEY_WIFI_SSID, settings->wifi_ssid, &ssid_len);
    size_t pwd_len = sizeof(settings->wifi_password);
    nvs_get_str(handle, KEY_WIFI_PASSWORD, settings->wifi_password, &pwd_len);
    uint8_t dhcp = 1;
    nvs_get_u8(handle, KEY_WIFI_DHCP, &dhcp);
    settings->wifi_dhcp = dhcp != 0;
    size_t ip_len = sizeof(settings->wifi_ip);
    nvs_get_str(handle, KEY_WIFI_IP, settings->wifi_ip, &ip_len);
    size_t netmask_len = sizeof(settings->wifi_netmask);
    nvs_get_str(handle, KEY_WIFI_NETMASK, settings->wifi_netmask, &netmask_len);
    size_t gw_len = sizeof(settings->wifi_gateway);
    nvs_get_str(handle, KEY_WIFI_GATEWAY, settings->wifi_gateway, &gw_len);
    size_t dns_len = sizeof(settings->wifi_dns);
    nvs_get_str(handle, KEY_WIFI_DNS, settings->wifi_dns, &dns_len);
}

static void settings_read_common_fields(nvs_handle_t handle, device_settings_t *settings)
{
    uint8_t onboarded = 0;
    nvs_get_u8(handle, KEY_ONBOARDED, &onboarded);
    settings->onboarding_complete = onboarded != 0;
    nvs_get_u8(handle, KEY_BRIGHTNESS, &settings->display_brightness);
    nvs_get_u16(handle, KEY_SLEEP_SECONDS, &settings->sleep_timeout_seconds);
    size_t name_length = sizeof(settings->device_name);
    nvs_get_str(handle, KEY_DEVICE_NAME, settings->device_name, &name_length);
    size_t layout_len = sizeof(settings->keyboard_layout);
    nvs_get_str(handle, KEY_KEYBOARD_LAYOUT, settings->keyboard_layout, &layout_len);
    uint8_t portal_auth = 0;
    nvs_get_u8(handle, "portal_auth", &portal_auth);
    settings->require_portal_auth = portal_auth != 0;
    settings_read_wifi_fields(handle, settings);
}

/**
 * Load settings from NVS into `settings`. If NVS is unavailable or the
 * stored version mismatches, defaults are returned and no error is raised.
 */
esp_err_t settings_load(device_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    settings_defaults(settings);

    nvs_handle_t handle;
    esp_err_t result = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (result != ESP_OK) {
        return result;
    }

    /* Version check with migration from v2/v3/v4 → v5. */
    uint8_t version = 0;
    result = nvs_get_u8(handle, KEY_VERSION, &version);
    if (result != ESP_OK) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (version != SETTINGS_VERSION && version != 2 && version != 3 && version != 4 && version != 5) {
        nvs_close(handle);
        return ESP_OK;
    }

    settings_read_common_fields(handle, settings);

    if (version >= 3) {
        uint8_t net_mode = SETTINGS_NETWORK_DIRECT;
        nvs_get_u8(handle, KEY_NETWORK_MODE, &net_mode);
        settings->network_mode = net_mode ? SETTINGS_NETWORK_HOME : SETTINGS_NETWORK_DIRECT;
        size_t ap_ssid_len = sizeof(settings->hotspot_ssid);
        nvs_get_str(handle, KEY_HOTSPOT_SSID, settings->hotspot_ssid, &ap_ssid_len);
        size_t ap_pwd_len = sizeof(settings->hotspot_password);
        nvs_get_str(handle, KEY_HOTSPOT_PASSWORD, settings->hotspot_password, &ap_pwd_len);
    } else {
        settings->network_mode = settings->wifi_ssid[0] != '\0'
            ? SETTINGS_NETWORK_HOME
            : SETTINGS_NETWORK_DIRECT;
    }

    if (version >= 4) {
        uint8_t auto_bak = 0;
        nvs_get_u8(handle, KEY_AUTO_BACKUP, &auto_bak);
        settings->auto_backup_on_connect = auto_bak != 0;
    }

    if (version >= 5) {
        size_t label_len = sizeof(settings->neo_label);
        nvs_get_str(handle, KEY_NEO_LABEL, settings->neo_label, &label_len);
    }

    if (version >= 6) {
        uint8_t auto_cloud = 0;
        nvs_get_u8(handle, KEY_AUTO_CLOUD, &auto_cloud);
        settings->auto_cloud_sync_after_backup = auto_cloud != 0;
    }

    settings_apply_hotspot_defaults(settings);
    nvs_close(handle);
    return ESP_OK;
}

/**
 * Persist the provided settings to NVS. Performs basic validation.
 */
esp_err_t settings_save(const device_settings_t *settings)
{
    if (settings == NULL || settings->display_brightness > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t result = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }

    /* Write version first so partial writes are detectable by the reader. */
    result = nvs_set_u8(handle, KEY_VERSION, SETTINGS_VERSION);
    if (result == ESP_OK) result = nvs_set_u8(handle, KEY_ONBOARDED, settings->onboarding_complete ? 1 : 0);
    if (result == ESP_OK) result = nvs_set_u8(handle, KEY_BRIGHTNESS, settings->display_brightness);
    if (result == ESP_OK) result = nvs_set_u16(handle, KEY_SLEEP_SECONDS, settings->sleep_timeout_seconds);
    if (result == ESP_OK) result = nvs_set_str(handle, KEY_DEVICE_NAME, settings->device_name);
    if (result == ESP_OK) result = nvs_set_str(handle, KEY_WIFI_SSID, settings->wifi_ssid);
    if (result == ESP_OK) result = nvs_set_str(handle, KEY_WIFI_PASSWORD, settings->wifi_password);
    if (result == ESP_OK) result = nvs_set_u8(handle, KEY_WIFI_DHCP, settings->wifi_dhcp ? 1 : 0);
    if (result == ESP_OK) result = nvs_set_str(handle, KEY_WIFI_IP, settings->wifi_ip);
    if (result == ESP_OK) result = nvs_set_str(handle, KEY_WIFI_NETMASK, settings->wifi_netmask);
    if (result == ESP_OK) result = nvs_set_str(handle, KEY_WIFI_GATEWAY, settings->wifi_gateway);
    if (result == ESP_OK) result = nvs_set_str(handle, KEY_WIFI_DNS, settings->wifi_dns);
    if (result == ESP_OK) result = nvs_set_str(handle, KEY_KEYBOARD_LAYOUT, settings->keyboard_layout);
    if (result == ESP_OK) result = nvs_set_u8(handle, "portal_auth", settings->require_portal_auth ? 1 : 0);
    if (result == ESP_OK) {
        result = nvs_set_u8(handle, KEY_NETWORK_MODE,
                            settings->network_mode == SETTINGS_NETWORK_HOME ? 1 : 0);
    }
    if (result == ESP_OK) result = nvs_set_str(handle, KEY_HOTSPOT_SSID, settings->hotspot_ssid);
    if (result == ESP_OK) result = nvs_set_str(handle, KEY_HOTSPOT_PASSWORD, settings->hotspot_password);
    if (result == ESP_OK) {
        result = nvs_set_u8(handle, KEY_AUTO_BACKUP, settings->auto_backup_on_connect ? 1 : 0);
    }
    if (result == ESP_OK) {
        result = nvs_set_u8(handle, KEY_AUTO_CLOUD, settings->auto_cloud_sync_after_backup ? 1 : 0);
    }
    if (result == ESP_OK) result = nvs_set_str(handle, KEY_NEO_LABEL, settings->neo_label);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

/* Convenience accessors used by UI code that prefer to read a single
 * property without allocating a settings structure. These load settings
 * each call, which is acceptable for low-frequency UI operations. */
const char *settings_get_device_name(void)
{
    static device_settings_t s;
    if (settings_load(&s) == ESP_OK) return s.device_name;
    return "Neo2 Buddy";
}

const char *settings_get_neo_label(void)
{
    static device_settings_t s;
    if (settings_load(&s) == ESP_OK) {
        return s.neo_label;
    }
    return "";
}

const char *settings_get_backup_label(void)
{
    /* Prefer neo_label so backups name the Neo/classroom, not only the buddy board. */
    static device_settings_t s;
    if (settings_load(&s) == ESP_OK) {
        if (s.neo_label[0] != '\0') {
            return s.neo_label;
        }
        if (s.device_name[0] != '\0') {
            return s.device_name;
        }
    }
    return "Neo2 Buddy";
}

const char *settings_get_keyboard_layout(void)
{
    static device_settings_t s;
    if (settings_load(&s) == ESP_OK) return s.keyboard_layout;
    return "u";
}

const char *settings_get_wifi_ssid(void)
{
    static device_settings_t s;
    if (settings_load(&s) == ESP_OK) return s.wifi_ssid[0] ? s.wifi_ssid : NULL;
    return NULL;
}