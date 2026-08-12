/**
 * @file settings.h
 * @brief Public API for persistent device settings.
 *
 * Provides a small, versioned settings structure persisted to NVS and a few
 * convenience helpers used by the web UI and other services. The settings
 * structure is intentionally compact to fit easily in NVS blobs.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SETTINGS_DEVICE_NAME_MAX_LENGTH 32
#define SETTINGS_WIFI_SSID_MAX_LENGTH 32
#define SETTINGS_WIFI_PASSWORD_MAX_LENGTH 64
/** Saved STA networks (home router, phone hotspot, …). */
#define SETTINGS_WIFI_NETWORK_MAX 4

#define SETTINGS_IP_STR_MAX_LENGTH 48

/** How the buddy reaches the admin portal. */
typedef enum {
    SETTINGS_NETWORK_DIRECT = 0, /**< Device hotspot at 192.168.4.1 (Direct access). */
    SETTINGS_NETWORK_HOME = 1,   /**< Join the user's home Wi‑Fi router. */
} settings_network_mode_t;

/** One remembered STA credential pair. */
typedef struct {
    char ssid[SETTINGS_WIFI_SSID_MAX_LENGTH + 1];
    char password[SETTINGS_WIFI_PASSWORD_MAX_LENGTH + 1];
} settings_wifi_network_t;

/** Compact device settings persisted to NVS. */
typedef struct {
    bool onboarding_complete;
    settings_network_mode_t network_mode;
    char hotspot_ssid[SETTINGS_WIFI_SSID_MAX_LENGTH + 1];
    char hotspot_password[SETTINGS_WIFI_PASSWORD_MAX_LENGTH + 1];
    uint8_t display_brightness;
    uint16_t sleep_timeout_seconds;
    char device_name[SETTINGS_DEVICE_NAME_MAX_LENGTH + 1];
    /** Preferred / last-used STA network (also mirrored in wifi_networks). */
    char wifi_ssid[SETTINGS_WIFI_SSID_MAX_LENGTH + 1];
    char wifi_password[SETTINGS_WIFI_PASSWORD_MAX_LENGTH + 1];
    uint8_t wifi_network_count;
    settings_wifi_network_t wifi_networks[SETTINGS_WIFI_NETWORK_MAX];
    bool wifi_dhcp;
    char wifi_ip[SETTINGS_IP_STR_MAX_LENGTH];
    char wifi_netmask[SETTINGS_IP_STR_MAX_LENGTH];
    char wifi_gateway[SETTINGS_IP_STR_MAX_LENGTH];
    char wifi_dns[SETTINGS_IP_STR_MAX_LENGTH];
    char keyboard_layout[8]; // e.g. "us", "nl"
    bool require_portal_auth; /* when true, require login to view portal */
    /** When Neo connects as keyboard, flip → backup changed AlphaWord → keyboard. */
    bool auto_backup_on_connect;
    /** After a successful local backup, start cloud sync when configured. */
    bool auto_cloud_sync_after_backup;
    /**
     * Optional label embedded in backup filenames (which physical Neo / classroom).
     * Empty → settings_get_backup_label() falls back to device_name.
     * Neo itself has no MAC/serial we can use for this.
     */
    char neo_label[SETTINGS_DEVICE_NAME_MAX_LENGTH + 1];
} device_settings_t;

/** Populate `settings` with safe defaults. */
void settings_defaults(device_settings_t *settings);
/** Load settings from NVS; on mismatch defaults are returned. */
esp_err_t settings_load(device_settings_t *settings);
/** Persist provided settings to NVS. */
esp_err_t settings_save(const device_settings_t *settings);

/* Convenience getters */
const char *settings_get_device_name(void);
/** Neo label for backup filenames; empty string if unset. */
const char *settings_get_neo_label(void);
/** Prefer neo_label, else device_name (never NULL). */
const char *settings_get_backup_label(void);
const char *settings_get_keyboard_layout(void);
const char *settings_get_wifi_ssid(void);
/** Fill hotspot SSID/password from device name when unset. */
void settings_apply_hotspot_defaults(device_settings_t *settings);

/**
 * Ensure wifi_ssid/password appear in wifi_networks (migration + primary sync).
 * Does not persist — caller must settings_save().
 */
void settings_wifi_sync_primary(device_settings_t *settings);

/**
 * Add or update a saved network and mark it preferred.
 * Empty password keeps the existing password when updating.
 * Returns false if the list is full and ssid is new.
 */
bool settings_wifi_upsert(device_settings_t *settings, const char *ssid, const char *password);

/** Remove a saved network by SSID. Updates preferred if needed. */
bool settings_wifi_remove(device_settings_t *settings, const char *ssid);

/** Look up a saved password for ssid (NULL if unknown). */
const char *settings_wifi_password_for(const device_settings_t *settings, const char *ssid);

/** Mark ssid preferred using the password stored for that network. */
void settings_wifi_set_preferred(device_settings_t *settings, const char *ssid);