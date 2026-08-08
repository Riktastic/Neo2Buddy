/**
 * @file web_api_http.c
 * @brief HTTP API for device management and the web UI.
 *
 * This module registers the management HTTP endpoints used by the single-
 * page web UI (served from `web/`) and provides handlers for authentication,
 * device status, file/import flows and simple command passthroughs to the
 * connected AlphaSmart NEO device.
 *
 * Current limitations and TODOs:
 * - JSON parsing uses simple string operations; replace with a proper
 *   JSON parser for robustness and security.
 * - Add OpenAPI/Swagger documentation for each endpoint.
 * - Add unit tests for handlers and request parsing.
 * - Implement rate-limiting, CSRF protections and stronger auth policies.
 */

#include "web_api_http.h"
#include "device_status.h"
#include "sd_format.h"
#include "usb_host_neo.h"
#include "neo_import.h"
#include "neo_live.h"
#include "settings.h"
#include "cloud_sync.h"
#include "factory_reset.h"
#include "board_config.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "esp_timer.h"
#include "auth.h"
#include "log_buffer.h"
#include "cJSON.h"
#include "hid_debug.h"
#include "neo_debug.h"
#include "health_check.h"
#include "wifi_manager.h"
#include "neo_applet.h"
#include "neo_file.h"
#include "neo_conv.h"
#include "neo_settings.h"
#include "neo_space.h"
#include "esp_wifi.h"
#include "sd_card.h"
#include "neo_import.h"
#include "neo_autobackup.h"
#include "file_manager.h"
#include "ble_hid.h"
#include "display.h"
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Rollback support: when applying a static IP we start a short timer. If the
    web UI cannot confirm reachability by POSTing to /api/v1/wifi/confirm before
    the timer fires, we restore the previous network settings to avoid device
    lockout. */
static bool wifi_rollback_pending = false;
static esp_timer_handle_t wifi_rollback_timer = NULL;
static esp_timer_handle_t onboarding_reboot_timer = NULL;
static int64_t s_boot_time_us = 0;

void web_api_note_boot_time(void)
{
    s_boot_time_us = esp_timer_get_time();
}

static bool reboot_cooldown_elapsed(void)
{
    if (s_boot_time_us == 0) {
        return true;
    }
    return (esp_timer_get_time() - s_boot_time_us) > (20 * 1000000LL);
}
static device_settings_t wifi_prev_settings;
static void wifi_rollback_timer_cb(void* arg);
static void onboarding_reboot_timer_cb(void *arg);

static void onboarding_reboot_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI("web_api", "Rebooting to apply network settings");
    esp_restart();
}

static void schedule_device_reboot(void)
{
    if (onboarding_reboot_timer) {
        esp_timer_stop(onboarding_reboot_timer);
        esp_timer_delete(onboarding_reboot_timer);
        onboarding_reboot_timer = NULL;
    }
    const esp_timer_create_args_t args = {
        .callback = onboarding_reboot_timer_cb,
        .name = "onboard_reboot",
    };
    if (esp_timer_create(&args, &onboarding_reboot_timer) == ESP_OK) {
        esp_timer_start_once(onboarding_reboot_timer, 3000 * 1000);
    } else {
        esp_restart();
    }
}

static bool parse_network_mode_value(const char *value, settings_network_mode_t *out)
{
    if (!value || !out) {
        return false;
    }
    if (strcmp(value, "home") == 0) {
        *out = SETTINGS_NETWORK_HOME;
        return true;
    }
    if (strcmp(value, "direct") == 0) {
        *out = SETTINGS_NETWORK_DIRECT;
        return true;
    }
    return false;
}

static void apply_network_json_fields(cJSON *root, device_settings_t *s)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "network_mode");
    if (cJSON_IsString(item) && item->valuestring) {
        settings_network_mode_t mode = s->network_mode;
        if (parse_network_mode_value(item->valuestring, &mode)) {
            s->network_mode = mode;
        }
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "hotspot_ssid");
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
        strncpy(s->hotspot_ssid, item->valuestring, sizeof(s->hotspot_ssid) - 1);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "hotspot_password");
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
        strncpy(s->hotspot_password, item->valuestring, sizeof(s->hotspot_password) - 1);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "wifi_ssid");
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
        strncpy(s->wifi_ssid, item->valuestring, sizeof(s->wifi_ssid) - 1);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "wifi_password");
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
        strncpy(s->wifi_password, item->valuestring, sizeof(s->wifi_password) - 1);
    }
    settings_apply_hotspot_defaults(s);
}

/** Validate network settings before save; writes a short error to err when invalid. */
static bool settings_network_config_valid(const device_settings_t *s, char *err, size_t err_len)
{
    if (s == NULL) {
        return false;
    }
    device_settings_t tmp = *s;
    settings_apply_hotspot_defaults(&tmp);
    if (tmp.network_mode == SETTINGS_NETWORK_HOME) {
        if (tmp.wifi_ssid[0] == '\0') {
            if (err && err_len > 0) {
                strlcpy(err, "home network mode requires a Wi-Fi network", err_len);
            }
            return false;
        }
        return true;
    }
    if (strlen(tmp.hotspot_password) < 8) {
        if (err && err_len > 0) {
            strlcpy(err, "hotspot password must be at least 8 characters", err_len);
        }
        return false;
    }
    if (tmp.hotspot_ssid[0] == '\0') {
        if (err && err_len > 0) {
            strlcpy(err, "hotspot name is required", err_len);
        }
        return false;
    }
    return true;
}

static void wifi_rollback_timer_cb(void* arg)
{
    if (!wifi_rollback_pending) return;
    log_buffer_appendf("wifi: rollback timer fired, restoring previous network settings");
    /* restore previous settings in NVS so the device boots back to previous state */
    settings_save(&wifi_prev_settings);
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif) {
        if (wifi_prev_settings.wifi_dhcp) {
            esp_netif_dhcpc_start(sta_netif);
            log_buffer_appendf("wifi: restored DHCP mode");
        } else if (wifi_prev_settings.wifi_ip[0] != '\0') {
            ip4_addr_t ip4, gw4, nm4;
            if (ip4addr_aton(wifi_prev_settings.wifi_ip, &ip4) && ip4addr_aton(wifi_prev_settings.wifi_gateway, &gw4) && ip4addr_aton(wifi_prev_settings.wifi_netmask, &nm4)) {
                esp_netif_ip_info_t ip_info;
                ip_info.ip.addr = ip4.addr;
                ip_info.gw.addr = gw4.addr;
                ip_info.netmask.addr = nm4.addr;
                esp_netif_dhcpc_stop(sta_netif);
                if (esp_netif_set_ip_info(sta_netif, &ip_info) == ESP_OK) {
                    log_buffer_appendf("wifi: restored static ip %s", wifi_prev_settings.wifi_ip);
                }
                if (wifi_prev_settings.wifi_dns[0] != '\0') {
                    ip4_addr_t dns4;
                    if (ip4addr_aton(wifi_prev_settings.wifi_dns, &dns4)) {
                        esp_netif_dns_info_t dns_info;
                        dns_info.ip.u_addr.ip4.addr = dns4.addr;
                        dns_info.ip.type = IPADDR_TYPE_V4;
                        esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info);
                        log_buffer_appendf("wifi: restored dns %s", wifi_prev_settings.wifi_dns);
                    }
                }
            }
        }
    }
    wifi_manager_connect(wifi_prev_settings.wifi_ssid, wifi_prev_settings.wifi_password);
    wifi_rollback_pending = false;
    if (wifi_rollback_timer) {
        esp_timer_delete(wifi_rollback_timer);
        wifi_rollback_timer = NULL;
    }
}

static esp_err_t login_post_handler(httpd_req_t *req);
static esp_err_t logout_post_handler(httpd_req_t *req);
static esp_err_t auth_password_post_handler(httpd_req_t *req);
static esp_err_t token_refresh_post_handler(httpd_req_t *req);
static void register_command_endpoints(httpd_handle_t server);
static esp_err_t keyboard_recent_get_handler(httpd_req_t *req);
static esp_err_t keyboard_clear_post_handler(httpd_req_t *req);
static esp_err_t keyboard_raw_get_handler(httpd_req_t *req);
static esp_err_t files_get_handler(httpd_req_t *req);
static esp_err_t files_post_handler(httpd_req_t *req);
static esp_err_t files_delete_handler(httpd_req_t *req);
static esp_err_t files_patch_handler(httpd_req_t *req);
static esp_err_t files_download_handler(httpd_req_t *req);
static esp_err_t files_view_handler(httpd_req_t *req);
static esp_err_t ble_get_handler(httpd_req_t *req);
static esp_err_t ble_pairing_post_handler(httpd_req_t *req);
static esp_err_t ble_preview_post_handler(httpd_req_t *req);
static esp_err_t ble_send_post_handler(httpd_req_t *req);
static esp_err_t ble_cancel_post_handler(httpd_req_t *req);
static esp_err_t logs_get_handler(httpd_req_t *req);
static esp_err_t settings_get_handler(httpd_req_t *req);
static esp_err_t sync_config_get_handler(httpd_req_t *req);
static esp_err_t sync_config_put_handler(httpd_req_t *req);
static esp_err_t sync_test_post_handler(httpd_req_t *req);
static esp_err_t sync_run_post_handler(httpd_req_t *req);
static esp_err_t factory_reset_post_handler(httpd_req_t *req);
static esp_err_t settings_post_handler(httpd_req_t *req);
static esp_err_t sd_status_get_handler(httpd_req_t *req);
static esp_err_t sd_format_post_handler(httpd_req_t *req);
static esp_err_t wifi_get_handler(httpd_req_t *req);
static esp_err_t wifi_post_handler(httpd_req_t *req);
static esp_err_t wifi_confirm_post_handler(httpd_req_t *req);
static esp_err_t wifi_scan_handler(httpd_req_t *req);
static esp_err_t static_get_handler(httpd_req_t *req);
static esp_err_t captive_check_get_handler(httpd_req_t *req);
static esp_err_t onboarding_get_handler(httpd_req_t *req);
static esp_err_t onboarding_post_handler(httpd_req_t *req);

static bool device_needs_onboarding(void)
{
    device_settings_t ds;
    if (settings_load(&ds) != ESP_OK) {
        return true;
    }
    return !ds.onboarding_complete;
}

static bool device_allows_setup_access(void)
{
    return device_needs_onboarding() || wifi_manager_is_recovery_mode();
}

static esp_err_t redirect_to_setup(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/setup.html");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/** Public read of onboarding state (no auth) for the setup page. */
static esp_err_t onboarding_get_handler(httpd_req_t *req)
{
    device_settings_t s;
    if (settings_load(&s) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "error", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "mem", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    cJSON_AddBoolToObject(root, "onboarding_complete", s.onboarding_complete);
    cJSON_AddBoolToObject(root, "wifi_recovery", wifi_manager_is_recovery_mode());
    cJSON_AddStringToObject(root, "device_name", s.device_name);
    settings_apply_hotspot_defaults(&s);
    cJSON_AddStringToObject(root, "network_mode",
                            s.network_mode == SETTINGS_NETWORK_HOME ? "home" : "direct");
    cJSON_AddStringToObject(root, "hotspot_ssid", s.hotspot_ssid);
    if (s.wifi_ssid[0] != '\0') {
        cJSON_AddStringToObject(root, "wifi_ssid", s.wifi_ssid);
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "json failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

/** Save first-run setup without auth while onboarding is incomplete. */
static esp_err_t onboarding_post_handler(httpd_req_t *req)
{
    device_settings_t s;
    if (settings_load(&s) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "error", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    bool recovery = wifi_manager_is_recovery_mode();
    if (s.onboarding_complete && !recovery) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_send(req, "already complete", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 1024) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "bad body", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char *buf = malloc((size_t)total_len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    int rlen = httpd_req_recv(req, buf, total_len);
    if (rlen <= 0) {
        free(buf);
        return ESP_FAIL;
    }
    buf[rlen] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "invalid json", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "device_name");
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(s.device_name, item->valuestring, sizeof(s.device_name) - 1);
    }
    apply_network_json_fields(root, &s);

    if (!recovery) {
        s.onboarding_complete = true;
        item = cJSON_GetObjectItemCaseSensitive(root, "onboarding_complete");
        if (cJSON_IsBool(item) && cJSON_IsFalse(item)) {
            s.onboarding_complete = false;
        }
        /* Production default: portal requires sign-in after first setup. */
        s.require_portal_auth = true;
        item = cJSON_GetObjectItemCaseSensitive(root, "require_portal_auth");
        if (cJSON_IsBool(item)) {
            s.require_portal_auth = cJSON_IsTrue(item);
        }
    }

    const cJSON *portal_pw = cJSON_GetObjectItemCaseSensitive(root, "portal_password");
    const char *portal_password = NULL;
    if (cJSON_IsString(portal_pw) && portal_pw->valuestring) {
        portal_password = portal_pw->valuestring;
    }
    if (!recovery) {
        if (!portal_password || strlen(portal_password) < 8 || strlen(portal_password) >= 64) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_send(req, "portal_password must be 8-63 characters", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
    }

    if (s.network_mode == SETTINGS_NETWORK_HOME) {
        if (s.wifi_ssid[0] == '\0') {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_send(req, "home network mode requires wifi_ssid", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
    } else {
        s.network_mode = SETTINGS_NETWORK_DIRECT;
        s.wifi_ssid[0] = '\0';
        s.wifi_password[0] = '\0';
        settings_apply_hotspot_defaults(&s);
    }

    char err_msg[96] = {0};
    if (!settings_network_config_valid(&s, err_msg, sizeof(err_msg))) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, err_msg[0] ? err_msg : "invalid network settings", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (portal_password && auth_set_password(portal_password) != ESP_OK) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "password save failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    cJSON_Delete(root);

    if (settings_save(&s) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "save failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"rebooting\":true}", HTTPD_RESP_USE_STRLEN);
    schedule_device_reboot();
    return ESP_OK;
}

/**
 * @brief Check whether the incoming HTTP request is authenticated.
 *
 * This helper inspects the `Authorization` header for a Bearer token and
 * validates it using the `auth` service.
 *
 * @param req The HTTP request to inspect.
 * @return true when authenticated, false otherwise.
 */
static bool request_is_authenticated(httpd_req_t *req)
{
    char auth_header[128];
    const char *prefix = "Bearer ";
    return httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) == ESP_OK &&
           strncmp(auth_header, prefix, strlen(prefix)) == 0 &&
           auth_check_token(auth_header + strlen(prefix));
}

/* Set common security headers for sensitive responses */
static void set_security_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
}

/**
 * @brief Send a minimal 401 Unauthorized response.
 *
 * Convenience used by handlers that failed authentication.
 *
 * @param req The HTTP request to respond to.
 * @return ESP_OK after sending the response.
 */
static esp_err_t send_unauthorized(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief Handle POST /api/v1/login
 *
 * Expects a JSON body containing a `password` field. On success returns a
 * JSON object containing a session token. This handler uses a naive string
 * parse and delegates credential checks to `auth_login`.
 *
 * @param req The HTTP request containing the login payload.
 * @return ESP_OK on normal completion; the HTTP status code indicates success.
 */
static esp_err_t login_post_handler(httpd_req_t *req) {
    if (auth_login_rate_limited()) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_send(req, "rate_limited", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 512) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "bad body", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char *buf = malloc(total_len + 1);
    int rlen = httpd_req_recv(req, buf, total_len);
    if (rlen <= 0) { free(buf); return ESP_FAIL; }
    buf[rlen] = '\0';
    // Parse JSON robustly with cJSON
    char password[128] = {0};
    cJSON *root = cJSON_Parse(buf);
    if (root) {
        cJSON *pw = cJSON_GetObjectItemCaseSensitive(root, "password");
        if (cJSON_IsString(pw) && (pw->valuestring != NULL)) {
            strncpy(password, pw->valuestring, sizeof(password)-1);
        }
        cJSON_Delete(root);
    } else {
        free(buf);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "invalid json", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char token[65] = {0};
    esp_err_t ret = auth_login(password, token, sizeof(token));
    free(buf);
    if (ret == ESP_OK) {
        uint64_t exp = auth_get_token_expiry();
        uint64_t now = (uint64_t)esp_timer_get_time() / 1000000ULL;
        int expires_in = exp > now ? (int)(exp - now) : 0;
        char out[160];
        snprintf(out, sizeof(out), "{\"token\":\"%s\",\"expires_in\":%d}", token, expires_in);
        log_buffer_appendf("auth: login success");
        set_security_headers(req);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (auth_login_rate_limited()) {
        log_buffer_appendf("auth: login rate-limited");
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_send(req, "rate_limited", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    /* Attempt to capture client IP from forwarded headers for logging */
    char ip_hdr[64];
    if (httpd_req_get_hdr_value_str(req, "X-Forwarded-For", ip_hdr, sizeof(ip_hdr)) == ESP_OK) {
        log_buffer_appendf("auth: login failure from %s", ip_hdr);
    } else {
        log_buffer_appendf("auth: login failure");
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    set_security_headers(req);
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* POST /api/v1/token/refresh - rotate a valid token and return a new one. */
static esp_err_t token_refresh_post_handler(httpd_req_t *req)
{
    char auth_hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr, sizeof(auth_hdr)) != ESP_OK) return send_unauthorized(req);
    const char *prefix = "Bearer ";
    if (strncmp(auth_hdr, prefix, strlen(prefix)) != 0) return send_unauthorized(req);
    char newtok[65] = {0};
    if (auth_refresh(auth_hdr + strlen(prefix), newtok, sizeof(newtok)) != ESP_OK) return send_unauthorized(req);
    uint64_t exp = auth_get_token_expiry();
    uint64_t now = (uint64_t)esp_timer_get_time() / 1000000ULL;
    int expires_in = exp > now ? (int)(exp - now) : 0;
    char out[160];
    snprintf(out, sizeof(out), "{\"token\":\"%s\",\"expires_in\":%d}", newtok, expires_in);
    set_security_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET /api/v1/keyboard/raw - return recent raw HID reports (hex) */
static esp_err_t keyboard_raw_get_handler(httpd_req_t *req)
{
    int limit = 32;
    char q[64];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char lims[16];
        if (httpd_query_key_value(q, "limit", lims, sizeof(lims)) == ESP_OK) {
            int v = atoi(lims);
            if (v > 0 && v <= 128) limit = v;
        }
    }
    size_t out_size = (size_t)limit * 128 + 64;
    char *out = malloc(out_size);
    if (!out) { httpd_resp_set_status(req, "500 Internal Server Error"); httpd_resp_send(req, "mem", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
    if (hid_debug_get_json(out, out_size, limit) != ESP_OK) {
        free(out);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "hid debug unavailable", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    set_security_headers(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

/* GET /api/v1/neo/debug?limit=N — recent Neo USB / protocol trace (for diagnostics) */
static esp_err_t neo_debug_get_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) return send_unauthorized(req);
    int limit = 64;
    char q[64];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char lims[16];
        if (httpd_query_key_value(q, "limit", lims, sizeof(lims)) == ESP_OK) {
            int v = atoi(lims);
            if (v > 0 && v <= 128) limit = v;
        }
    }
    size_t out_size = (size_t)limit * 192 + 64;
    char *out = malloc(out_size);
    if (!out) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "mem", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (neo_debug_get_json(out, out_size, limit) != ESP_OK) {
        free(out);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "neo debug unavailable", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

/* POST /api/v1/neo/rescan — scan OTG1 and trigger Neo HID->comms flip if found */
static esp_err_t neo_rescan_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        ESP_LOGW("web_api", "POST /neo/rescan denied: not authenticated");
        return send_unauthorized(req);
    }

    ESP_LOGI("web_api", "POST /neo/rescan");
    neo_usb_scan_result_t scan = {0};
    esp_err_t err = usb_host_neo_rescan(&scan);
    if (err != ESP_OK) {
        ESP_LOGE("web_api", "Neo rescan failed: %s", esp_err_to_name(err));
        neo_debug_event("web rescan failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI("web_api", "Neo rescan ok bus=%d neo_ready=%d flipping=%d found=%d",
                 scan.bus_device_count, scan.neo_ready ? 1 : 0, scan.flipping ? 1 : 0,
                 scan.devices_found);
        neo_debug_event("web rescan ok bus=%d neo_ready=%d devices=%d", scan.bus_device_count,
                        scan.neo_ready ? 1 : 0, scan.devices_found);
    }
    char buf[384];
    if (err != ESP_OK) {
        snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
    } else {
        char devs[160] = "[";
        for (int i = 0; i < scan.devices_found && i < 4; i++) {
            char one[32];
            snprintf(one, sizeof(one), "%s{\"vid\":%u,\"pid\":%u}", i ? "," : "", scan.vid[i], scan.pid[i]);
            strncat(devs, one, sizeof(devs) - strlen(devs) - 1);
        }
        strncat(devs, "]", sizeof(devs) - strlen(devs) - 1);
        snprintf(buf, sizeof(buf),
                 "{\"ok\":true,\"bus_devices\":%d,\"neo_ready\":%s,\"flipping\":%s,\"devices\":%s}",
                 scan.bus_device_count, scan.neo_ready ? "true" : "false",
                 scan.flipping ? "true" : "false", devs);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET /api/v1/neo/files — list AlphaWord document slots (0xA000) */
static esp_err_t neo_require_connected(httpd_req_t *req);

static esp_err_t neo_files_get_handler(httpd_req_t *req)
{
    ESP_LOGI("web_api", "GET /neo/files");
    if (!request_is_authenticated(req)) {
        ESP_LOGW("web_api", "Neo file scan denied: not authenticated");
        return send_unauthorized(req);
    }
    if (neo_require_connected(req) != ESP_OK) {
        return ESP_OK;
    }

    ESP_LOGI("web_api", "Neo file scan requested");
    cJSON *files = cJSON_CreateArray();
    if (!files) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "mem", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    esp_err_t err = usb_host_neo_list_files(files);
    if (err != ESP_OK) {
        cJSON_Delete(files);
        char errbuf[128];
        snprintf(errbuf, sizeof(errbuf), "{\"error\":\"%s\"}", esp_err_to_name(err));
        ESP_LOGE("web_api", "Neo file scan failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, errbuf, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char *out = cJSON_PrintUnformatted(files);
    cJSON_Delete(files);
    if (!out) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "mem", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

static esp_err_t neo_require_connected(httpd_req_t *req)
{
    if (!usb_host_neo_is_connected()) {
        neo_usb_scan_result_t scan = {0};
        usb_host_neo_rescan(&scan);
    }
    if (!usb_host_neo_is_connected()) {
        httpd_resp_set_status(req, "412 Precondition Failed");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"neo_not_connected\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = usb_host_neo_ensure_comms();
    if (err != ESP_OK) {
        neo_usb_scan_result_t scan = {0};
        if (usb_host_neo_rescan(&scan) == ESP_OK) {
            err = usb_host_neo_ensure_comms();
        }
    }
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"neo_comms_unavailable\"}", HTTPD_RESP_USE_STRLEN);
        return err;
    }
    return ESP_OK;
}

/** Portal document/backup APIs only support AlphaWord (0xA000) text files. */
static esp_err_t neo_require_alphaword_applet(httpd_req_t *req, unsigned int applet_id)
{
    if (applet_id == (unsigned int)NEO_APPLET_ID_ALPHAWORD) {
        return ESP_OK;
    }
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"alphaword_only\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t neo_read_request_body(httpd_req_t *req, uint8_t **out_buf, size_t *out_len)
{
    int total_len = req->content_len;
    if (total_len <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = malloc((size_t)total_len);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    int received = 0;
    while (received < total_len) {
        int chunk = httpd_req_recv(req, (char *)buf + received, total_len - received);
        if (chunk <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        received += chunk;
    }
    *out_buf = buf;
    *out_len = (size_t)total_len;
    return ESP_OK;
}

static neo_charmap_id_t neo_charmap_from_query(httpd_req_t *req)
{
    char q[64];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return NEO_CHARMAP_EN_US;
    }
    char map_name[16];
    if (httpd_query_key_value(q, "map", map_name, sizeof(map_name)) != ESP_OK) {
        return NEO_CHARMAP_EN_US;
    }
    return neo_charmap_by_name(map_name);
}

static bool neo_query_has_backup(httpd_req_t *req)
{
    char q[64];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return false;
    }
    char flag[8];
    if (httpd_query_key_value(q, "backup", flag, sizeof(flag)) != ESP_OK) {
        return false;
    }
    return strcmp(flag, "1") == 0 || strcasecmp(flag, "true") == 0;
}

/* POST /api/v1/neo/applets — install SmartApplet package */
static esp_err_t neo_applets_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    if (neo_require_connected(req) != ESP_OK) {
        return ESP_OK;
    }

    uint8_t *body = NULL;
    size_t body_len = 0;
    esp_err_t err = neo_read_request_body(req, &body, &body_len);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing body", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    bool replace = false;
    char replace_hdr[8];
    if (httpd_req_get_hdr_value_str(req, "X-Neo-Replace", replace_hdr, sizeof(replace_hdr)) == ESP_OK) {
        replace = strcmp(replace_hdr, "true") == 0 || strcmp(replace_hdr, "1") == 0;
    }

    err = usb_host_neo_install_applet(body, body_len, replace);
    free(body);
    if (err != ESP_OK) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* DELETE /api/v1/neo/applets/{id} */
static esp_err_t neo_applet_delete_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    if (neo_require_connected(req) != ESP_OK) {
        return ESP_OK;
    }

    unsigned int applet_id = 0;
    if (sscanf(req->uri, "/api/v1/neo/applets/%u", &applet_id) != 1 || applet_id > 0xffff) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "bad applet id", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    esp_err_t err = usb_host_neo_remove_applet((uint16_t)applet_id);
    if (err != ESP_OK) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET /api/v1/neo/applets/{id}/download */
static esp_err_t neo_applet_download_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    if (neo_require_connected(req) != ESP_OK) {
        return ESP_OK;
    }

    unsigned int applet_id = 0;
    if (sscanf(req->uri, "/api/v1/neo/applets/%u/download", &applet_id) != 1 || applet_id > 0xffff) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "bad applet id", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    size_t cap = 512 * 1024;
    uint8_t *buf = malloc(cap);
    if (!buf) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "mem", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    size_t out_len = 0;
    esp_err_t err = usb_host_neo_fetch_applet((uint16_t)applet_id, buf, cap, &out_len);
    if (err != ESP_OK) {
        free(buf);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "fetch error", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char filename[64];
    snprintf(filename, sizeof(filename), "applet-%u.os3kapp", applet_id);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", filename);
    httpd_resp_send(req, (const char *)buf, out_len);
    free(buf);
    return ESP_OK;
}

static esp_err_t neo_file_transfer_handler(httpd_req_t *req, bool is_download)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    if (neo_require_connected(req) != ESP_OK) {
        return ESP_OK;
    }

    unsigned int applet_id = 0;
    unsigned int file_index = 0;
    char action[16];
    if (sscanf(req->uri, "/api/v1/neo/applets/%u/files/%u/%15s", &applet_id, &file_index, action) != 3 ||
        applet_id > 0xffff || file_index == 0 || file_index > 255) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "bad path", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (is_download && strcmp(action, "download") != 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "not found", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (!is_download && (strcmp(action, "read") != 0 || req->method != HTTP_POST)) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "not found", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (neo_require_alphaword_applet(req, applet_id) != ESP_OK) {
        return ESP_OK;
    }

    neo_file_attr_t attrs;
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    esp_err_t err = usb_host_neo_read_file_alloc((uint16_t)applet_id, (uint8_t)file_index, &attrs, &raw,
                                                 &raw_len, 256 * 1024);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "not found", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "read error", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    neo_charmap_id_t map = neo_charmap_from_query(req);
    size_t text_cap = neo_conv_export_buf_size(raw_len > 0 ? raw_len : 1);
    char *text = malloc(text_cap);
    if (!text) {
        free(raw);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "mem", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    size_t text_len = neo_conv_export_text_from_neo(raw ? raw : (const uint8_t *)"", raw_len, text, text_cap, map);

    if (!is_download && neo_query_has_backup(req)) {
        char saved_path[256];
        err = neo_import_save_raw_document(raw ? raw : (const uint8_t *)"", raw_len, attrs.name,
                                           (uint8_t)file_index, saved_path, sizeof(saved_path));
        free(raw);
        if (err != ESP_OK) {
            free(text);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_send(req, "save error", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        char out[640];
        snprintf(out, sizeof(out), "{\"saved\":true,\"path\":\"%s\"}", saved_path);
        free(text);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    free(raw);
    if (is_download) {
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_send(req, text, text_len);
    } else {
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_send(req, text, text_len);
    }
    free(text);
    return ESP_OK;
}

static esp_err_t neo_file_read_post_handler(httpd_req_t *req)
{
    return neo_file_transfer_handler(req, false);
}

static esp_err_t neo_file_download_get_handler(httpd_req_t *req)
{
    return neo_file_transfer_handler(req, true);
}

/* POST /api/v1/neo/applets/{id}/files/{index}/write — import UTF-8 text to Neo file */
static esp_err_t neo_file_write_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    if (neo_require_connected(req) != ESP_OK) {
        return ESP_OK;
    }

    unsigned int applet_id = 0;
    unsigned int file_index = 0;
    if (sscanf(req->uri, "/api/v1/neo/applets/%u/files/%u/write", &applet_id, &file_index) != 2 ||
        applet_id > 0xffff || file_index == 0 || file_index > 255) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "bad path", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (neo_require_alphaword_applet(req, applet_id) != ESP_OK) {
        return ESP_OK;
    }

    uint8_t *body = NULL;
    size_t body_len = 0;
    esp_err_t err = neo_read_request_body(req, &body, &body_len);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing body", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    neo_charmap_id_t map = neo_charmap_from_query(req);
    size_t neo_cap = body_len * 2 + 512;
    uint8_t *neo_buf = malloc(neo_cap);
    if (!neo_buf) {
        free(body);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "mem", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    size_t neo_len = 0;
    err = neo_conv_import_text_to_neo((const char *)body, map, neo_buf, neo_cap, &neo_len);
    free(body);
    if (err != ESP_OK) {
        free(neo_buf);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "convert error", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    err = usb_host_neo_write_file_raw((uint16_t)applet_id, (uint8_t)file_index, neo_buf, neo_len);
    free(neo_buf);
    if (err != ESP_OK) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* DELETE /api/v1/neo/applets/{id}/files/{index} — clear file */
static esp_err_t neo_file_clear_delete_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    if (neo_require_connected(req) != ESP_OK) {
        return ESP_OK;
    }

    unsigned int applet_id = 0;
    unsigned int file_index = 0;
    if (sscanf(req->uri, "/api/v1/neo/applets/%u/files/%u", &applet_id, &file_index) != 2 ||
        applet_id > 0xffff || file_index == 0 || file_index > 255) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "bad path", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (neo_require_alphaword_applet(req, applet_id) != ESP_OK) {
        return ESP_OK;
    }

    esp_err_t err = usb_host_neo_clear_file((uint16_t)applet_id, (uint8_t)file_index);
    if (err != ESP_OK) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* POST /api/v1/neo/restart — return Neo to keyboard mode (NeoTools flip-to-keyboard) */
static esp_err_t neo_restart_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    if (neo_require_connected(req) != ESP_OK) {
        return ESP_OK;
    }
    if (neo_autobackup_is_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"auto_backup_busy\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    esp_err_t err = usb_host_neo_restart();
    if (err != ESP_OK) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"mode\":\"keyboard\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET /api/v1/neo/autobackup — progress for portal/Python pollers.
 * POST starts async changed-file backup then return-to-keyboard (same product
 * path as connect auto-backup, but force=true so cooldown/setting do not block). */
static esp_err_t neo_autobackup_get_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    device_settings_t s;
    bool enabled = false;
    if (settings_load(&s) == ESP_OK) {
        enabled = s.auto_backup_on_connect;
    }
    bool busy = neo_autobackup_is_busy();
    neo_autobackup_progress_t prog;
    neo_autobackup_get_progress(&prog);
    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"auto_backup_on_connect\":%s,\"busy\":%s,\"last_result\":\"%s\","
             "\"phase\":\"%s\",\"current\":%u,\"total\":%u,\"saved\":%u,\"skipped\":%u}",
             enabled ? "true" : "false", busy ? "true" : "false",
             esp_err_to_name(neo_autobackup_last_result()), prog.phase, (unsigned)prog.current,
             (unsigned)prog.total, (unsigned)prog.saved, (unsigned)prog.skipped);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t neo_autobackup_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    if (neo_require_connected(req) != ESP_OK) {
        return ESP_OK;
    }
    esp_err_t err = neo_autobackup_start_async(true);
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"auto_backup_busy\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"started\":true,\"returned_to_keyboard\":true}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET|PUT /api/v1/sync/config — cloud backup destination (secrets redacted). */
static esp_err_t sync_config_get_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "mem", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (cloud_sync_json_config(root) != ESP_OK) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "sync config failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "json failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

static esp_err_t sync_config_put_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 2048) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "bad body", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char *buf = malloc((size_t)total_len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    int rlen = httpd_req_recv(req, buf, total_len);
    if (rlen <= 0) {
        free(buf);
        return ESP_FAIL;
    }
    buf[rlen] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "invalid json", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char err[128];
    esp_err_t err_code = cloud_sync_apply_config_json(root, err, sizeof(err));
    cJSON_Delete(root);
    if (err_code != ESP_OK) {
        char out[192];
        snprintf(out, sizeof(out), "{\"ok\":false,\"error\":\"%s\"}", err[0] ? err : esp_err_to_name(err_code));
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t sync_test_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    char message[160];
    esp_err_t err = cloud_sync_test(message, sizeof(message));
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "mem", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddStringToObject(root, "message", message);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "json failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "502 Bad Gateway");
    }
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

static esp_err_t sync_run_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    esp_err_t err = cloud_sync_start_run();
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"sync_busy\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"started\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* POST /api/v1/device/factory-reset — wipe NVS + internal backups, reboot. */
static esp_err_t factory_reset_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 512) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "bad body", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char *buf = malloc((size_t)total_len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    int rlen = httpd_req_recv(req, buf, total_len);
    if (rlen <= 0) {
        free(buf);
        return ESP_FAIL;
    }
    buf[rlen] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "invalid json", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    const cJSON *pw = cJSON_GetObjectItemCaseSensitive(root, "password");
    const char *password = cJSON_IsString(pw) ? pw->valuestring : NULL;
    cJSON_Delete(root);
    if (!password || password[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"password required\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (auth_login_rate_limited()) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"too many attempts\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (!auth_check_password(password)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"invalid password\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"rebooting\":true}", HTTPD_RESP_USE_STRLEN);
    factory_reset_execute();
    return ESP_OK;
}

/* DELETE /api/v1/neo/applets — remove all SmartApplets */
static esp_err_t neo_applets_delete_all_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    if (neo_require_connected(req) != ESP_OK) {
        return ESP_OK;
    }
    esp_err_t err = usb_host_neo_remove_all_applets();
    if (err != ESP_OK) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* POST /api/v1/neo/applets/inspect — parse local .os3kapp header without USB */
static esp_err_t neo_applet_inspect_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    uint8_t *body = NULL;
    size_t body_len = 0;
    esp_err_t err = neo_read_request_body(req, &body, &body_len);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing body", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char out[384];
    err = usb_host_neo_inspect_applet(body, body_len, out, sizeof(out));
    free(body);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"invalid package\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static bool neo_query_target(httpd_req_t *req, char *target, size_t target_size)
{
    char q[128];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(q, "target", target, target_size) == ESP_OK && target[0] != '\0';
}

/* POST /api/v1/neo/applets/{id}/files/read-all — full applet backup, then keyboard.
 * Deliberately different from autobackup: every non-empty file, not "changed only". */
static esp_err_t neo_files_read_all_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    if (neo_require_connected(req) != ESP_OK) {
        return ESP_OK;
    }
    if (neo_autobackup_is_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"auto_backup_busy\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    unsigned int applet_id = 0;
    if (sscanf(req->uri, "/api/v1/neo/applets/%u/files/read-all", &applet_id) != 1 || applet_id > 0xffff) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "bad path", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (neo_require_alphaword_applet(req, applet_id) != ESP_OK) {
        return ESP_OK;
    }
    neo_charmap_id_t map = neo_charmap_from_query(req);
    cJSON *saved = cJSON_CreateArray();
    if (!saved) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "mem", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    esp_err_t err = usb_host_neo_backup_all_files((uint16_t)applet_id, map, saved);
    if (err != ESP_OK) {
        cJSON_Delete(saved);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "read-all failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    bool returned_to_keyboard = false;
    esp_err_t restart_err = usb_host_neo_restart();
    if (restart_err == ESP_OK) {
        returned_to_keyboard = true;
    } else {
        neo_debug_event("web read-all restart(keyboard) failed: %s", esp_err_to_name(restart_err));
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "saved", saved);
    cJSON_AddNumberToObject(root, "count", cJSON_GetArraySize(saved));
    cJSON_AddBoolToObject(root, "returned_to_keyboard", returned_to_keyboard);
    if (!returned_to_keyboard) {
        cJSON_AddStringToObject(root, "restart_error", esp_err_to_name(restart_err));
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "mem", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

/* POST /api/v1/neo/applets/{id}/files/write?target=name_or_space — write or create by name/space */
static esp_err_t neo_file_by_name_write_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    if (neo_require_connected(req) != ESP_OK) {
        return ESP_OK;
    }
    unsigned int applet_id = 0;
    if (sscanf(req->uri, "/api/v1/neo/applets/%u/files/write", &applet_id) != 1 || applet_id > 0xffff) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "bad path", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (neo_require_alphaword_applet(req, applet_id) != ESP_OK) {
        return ESP_OK;
    }
    char target[32];
    if (!neo_query_target(req, target, sizeof(target))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing target", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char password[16] = "write";
    char q[64];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        httpd_query_key_value(q, "password", password, sizeof(password));
    }
    uint8_t *body = NULL;
    size_t body_len = 0;
    esp_err_t err = neo_read_request_body(req, &body, &body_len);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing body", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    neo_charmap_id_t map = neo_charmap_from_query(req);
    size_t neo_cap = body_len * 2 + 512;
    uint8_t *neo_buf = malloc(neo_cap);
    if (!neo_buf) {
        free(body);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "mem", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    size_t neo_len = 0;
    err = neo_conv_import_text_to_neo((const char *)body, map, neo_buf, neo_cap, &neo_len);
    free(body);
    if (err != ESP_OK) {
        free(neo_buf);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "convert error", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    err = usb_host_neo_write_file_by_name((uint16_t)applet_id, target, password, neo_buf, neo_len);
    free(neo_buf);
    if (err != ESP_OK) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* DELETE /api/v1/neo/applets/{id}/files?target=name_or_space — clear by name/space */
static esp_err_t neo_file_by_name_clear_delete_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    if (neo_require_connected(req) != ESP_OK) {
        return ESP_OK;
    }
    unsigned int applet_id = 0;
    if (sscanf(req->uri, "/api/v1/neo/applets/%u/files", &applet_id) != 1 || applet_id > 0xffff) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "bad path", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (neo_require_alphaword_applet(req, applet_id) != ESP_OK) {
        return ESP_OK;
    }
    char target[32];
    if (!neo_query_target(req, target, sizeof(target))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing target", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    esp_err_t err = usb_host_neo_clear_file_by_name((uint16_t)applet_id, target);
    if (err != ESP_OK) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_set_status(req, err == ESP_ERR_NOT_FOUND ? "404 Not Found" : "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief Handle POST /api/v1/logout
 *
 * Validates the bearer token and invalidates it via `auth_logout`.
 *
 * @param req The HTTP request to process.
 * @return ESP_OK after sending the response.
 */
static esp_err_t logout_post_handler(httpd_req_t *req) {
    char auth_hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr, sizeof(auth_hdr)) != ESP_OK) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    const char *prefix = "Bearer ";
    if (strncmp(auth_hdr, prefix, strlen(prefix)) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (!auth_check_token(auth_hdr + strlen(prefix))) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    auth_logout();
    set_security_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* POST /api/v1/auth/password — change portal password (requires current password). */
static esp_err_t auth_password_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 512) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "bad body", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char *buf = malloc((size_t)total_len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    int rlen = httpd_req_recv(req, buf, total_len);
    if (rlen <= 0) {
        free(buf);
        return ESP_FAIL;
    }
    buf[rlen] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "invalid json", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "current_password");
    const cJSON *neu = cJSON_GetObjectItemCaseSensitive(root, "new_password");
    const char *current = cJSON_IsString(cur) ? cur->valuestring : NULL;
    const char *new_pw = cJSON_IsString(neu) ? neu->valuestring : NULL;
    cJSON_Delete(root);
    if (!current || !new_pw) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"current_password and new_password required\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    esp_err_t err = auth_change_password(current, new_pw);
    if (err == ESP_ERR_INVALID_SIZE) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"new password must be 8-63 characters\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"too many attempts\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"invalid current password\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    log_buffer_appendf("auth: portal password changed");
    set_security_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief Handle GET /api/v1/status
 *
 * Returns basic device connectivity and USB-attached NEO device info as
 * JSON. Authentication is required.
 *
 * @param req The HTTP request to respond to.
 * @return ESP_OK after sending the status JSON.
 */
static esp_err_t status_get_handler(httpd_req_t *req) {
    if (!request_is_authenticated(req)) return send_unauthorized(req);
    device_status_t st;
    device_status_get(&st);
    char buf[1024];
    bool conn = usb_host_neo_is_connected();
    neo_usb_dev_info_t dev;
    const char *ble_state = "idle";
    if (st.ble_state == DEVICE_BLE_PAIRING) ble_state = "pairing";
    else if (st.ble_state == DEVICE_BLE_CONNECTED) ble_state = "connected";

    neo_usb_host_status_t usb_st;
    usb_host_neo_get_host_status(&usb_st);
    const char *usb_port_hint =
        "Neo USB-B needs 5V (powerbank OK). Internal batteries alone won't enable USB/emulation";

    device_settings_t settings;
    bool auto_bak = false;
    if (settings_load(&settings) == ESP_OK) {
        auto_bak = settings.auto_backup_on_connect;
    }
    bool bak_busy = neo_autobackup_is_busy();
    neo_autobackup_progress_t bak_prog;
    neo_autobackup_get_progress(&bak_prog);

    if (conn && usb_host_neo_get_last_device_info(&dev) == ESP_OK) {
        snprintf(buf, sizeof(buf),
            "{\"usb_connected\":true,\"vid\":%d,\"pid\":%d,\"product\":\"%s\","
            "\"usb_host_active\":%s,\"usb_bus_devices\":%d,\"usb_neo_ready\":%s,"
            "\"usb_keyboard_active\":%s,\"usb_flipping\":%s,\"usb_port_hint\":\"%s\","
            "\"auto_backup_on_connect\":%s,\"auto_backup_busy\":%s,"
            "\"auto_backup_phase\":\"%s\",\"auto_backup_current\":%u,\"auto_backup_total\":%u,"
            "\"have_battery\":%s,\"have_sdcard\":%s,\"have_oled\":%s,"
            "\"battery_percent\":%u,\"charging\":%s,\"wifi_state\":%d,"
            "\"ble_state\":\"%s\",\"ble_can_send\":%s,\"ip\":\"%s\"}",
            dev.vendor_id, dev.product_id, dev.product_string,
            usb_st.host_installed ? "true" : "false", usb_st.bus_device_count,
            usb_st.neo_ready ? "true" : "false",
            usb_st.keyboard_active ? "true" : "false",
            usb_st.flipping ? "true" : "false", usb_port_hint,
            auto_bak ? "true" : "false", bak_busy ? "true" : "false",
            bak_prog.phase, (unsigned)bak_prog.current, (unsigned)bak_prog.total,
            HAVE_BATTERY ? "true" : "false",
            HAVE_SDCARD ? "true" : "false",
            HAVE_OLED ? "true" : "false",
            st.battery_percent, st.charging ? "true" : "false", (int)st.wifi_state,
            ble_state, ble_hid_can_send() ? "true" : "false", st.ip_address);
    } else {
        snprintf(buf, sizeof(buf),
            "{\"usb_connected\":false,\"usb_host_active\":%s,\"usb_bus_devices\":%d,"
            "\"usb_neo_ready\":%s,\"usb_keyboard_active\":%s,\"usb_flipping\":%s,\"usb_port_hint\":\"%s\","
            "\"auto_backup_on_connect\":%s,\"auto_backup_busy\":%s,"
            "\"auto_backup_phase\":\"%s\",\"auto_backup_current\":%u,\"auto_backup_total\":%u,"
            "\"have_battery\":%s,\"have_sdcard\":%s,\"have_oled\":%s,"
            "\"battery_percent\":%u,\"charging\":%s,\"wifi_state\":%d,"
            "\"ble_state\":\"%s\",\"ble_can_send\":%s,\"ip\":\"%s\"}",
            usb_st.host_installed ? "true" : "false", usb_st.bus_device_count,
            usb_st.neo_ready ? "true" : "false",
            usb_st.keyboard_active ? "true" : "false",
            usb_st.flipping ? "true" : "false", usb_port_hint,
            auto_bak ? "true" : "false", bak_busy ? "true" : "false",
            bak_prog.phase, (unsigned)bak_prog.current, (unsigned)bak_prog.total,
            HAVE_BATTERY ? "true" : "false",
            HAVE_SDCARD ? "true" : "false",
            HAVE_OLED ? "true" : "false",
            st.battery_percent, st.charging ? "true" : "false", (int)st.wifi_state,
            ble_state, ble_hid_can_send() ? "true" : "false", st.ip_address);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief Register all HTTP API endpoints with the running server.
 *
 * This function registers authentication endpoints, settings, keyboard
 * snapshot endpoints and command passthrough routes used by the web UI.
 *
 * @param server HTTP server handle returned by `httpd_start`.
 * @return ESP_OK on success, or the first error from `httpd_register_uri_handler`.
 */
esp_err_t web_api_register(httpd_handle_t server) {
    esp_err_t r;

    httpd_uri_t status_uri = {
        .uri = "/api/v1/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
        .user_ctx = NULL
    };
    r = httpd_register_uri_handler(server, &status_uri);
    if (r != ESP_OK) return r;

    /* Backwards-compatible path used by earlier portal previews. */
    httpd_uri_t status_uri_compat = {
        .uri = "/api/v1/usb/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
        .user_ctx = NULL
    };
    r = httpd_register_uri_handler(server, &status_uri_compat);
    if (r != ESP_OK) return r;

    // Login endpoint
    httpd_uri_t login_uri = {
        .uri = "/api/v1/login",
        .method = HTTP_POST,
        .handler = login_post_handler,
        .user_ctx = NULL
    };
    r = httpd_register_uri_handler(server, &login_uri);
    if (r != ESP_OK) return r;

    // Logout endpoint
    httpd_uri_t logout_uri = {
        .uri = "/api/v1/logout",
        .method = HTTP_POST,
        .handler = logout_post_handler,
        .user_ctx = NULL
    };
    r = httpd_register_uri_handler(server, &logout_uri);
    if (r != ESP_OK) return r;

    httpd_uri_t auth_password_uri = {
        .uri = "/api/v1/auth/password",
        .method = HTTP_POST,
        .handler = auth_password_post_handler,
        .user_ctx = NULL
    };
    r = httpd_register_uri_handler(server, &auth_password_uri);
    if (r != ESP_OK) return r;

    // Files listing
    httpd_uri_t files_uri = {
        .uri = "/api/v1/files",
        .method = HTTP_GET,
        .handler = files_get_handler,
        .user_ctx = NULL
    };
    r = httpd_register_uri_handler(server, &files_uri);
    if (r != ESP_OK) return r;

    httpd_uri_t files_post_uri = { .uri = "/api/v1/files", .method = HTTP_POST, .handler = files_post_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &files_post_uri); if (r != ESP_OK) return r;
    httpd_uri_t files_delete_uri = { .uri = "/api/v1/files", .method = HTTP_DELETE, .handler = files_delete_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &files_delete_uri); if (r != ESP_OK) return r;
    httpd_uri_t files_patch_uri = { .uri = "/api/v1/files", .method = HTTP_PATCH, .handler = files_patch_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &files_patch_uri); if (r != ESP_OK) return r;
    httpd_uri_t files_download_uri = { .uri = "/api/v1/files/download", .method = HTTP_GET, .handler = files_download_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &files_download_uri); if (r != ESP_OK) return r;
    httpd_uri_t files_view_uri = { .uri = "/api/v1/files/view", .method = HTTP_GET, .handler = files_view_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &files_view_uri); if (r != ESP_OK) return r;

    httpd_uri_t ble_get_uri = { .uri = "/api/v1/ble", .method = HTTP_GET, .handler = ble_get_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &ble_get_uri); if (r != ESP_OK) return r;
    httpd_uri_t ble_pair_uri = { .uri = "/api/v1/ble/pairing", .method = HTTP_POST, .handler = ble_pairing_post_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &ble_pair_uri); if (r != ESP_OK) return r;
    httpd_uri_t ble_preview_uri = { .uri = "/api/v1/ble/preview", .method = HTTP_POST, .handler = ble_preview_post_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &ble_preview_uri); if (r != ESP_OK) return r;
    httpd_uri_t ble_send_uri = { .uri = "/api/v1/ble/send", .method = HTTP_POST, .handler = ble_send_post_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &ble_send_uri); if (r != ESP_OK) return r;
    httpd_uri_t ble_cancel_uri = { .uri = "/api/v1/ble/cancel", .method = HTTP_POST, .handler = ble_cancel_post_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &ble_cancel_uri); if (r != ESP_OK) return r;

    httpd_uri_t keyboard_recent_uri = {
        .uri = "/api/v1/keyboard/recent", .method = HTTP_GET,
        .handler = keyboard_recent_get_handler, .user_ctx = NULL
    };
    r = httpd_register_uri_handler(server, &keyboard_recent_uri);
    if (r != ESP_OK) return r;

    httpd_uri_t keyboard_raw_uri = { .uri = "/api/v1/keyboard/raw", .method = HTTP_GET, .handler = keyboard_raw_get_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &keyboard_raw_uri); if (r != ESP_OK) return r;
    httpd_uri_t neo_debug_uri = { .uri = "/api/v1/neo/debug", .method = HTTP_GET, .handler = neo_debug_get_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &neo_debug_uri); if (r != ESP_OK) return r;
    httpd_uri_t neo_rescan_uri = { .uri = "/api/v1/neo/rescan", .method = HTTP_POST, .handler = neo_rescan_post_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &neo_rescan_uri); if (r != ESP_OK) return r;
    httpd_uri_t neo_files_uri = { .uri = "/api/v1/neo/files", .method = HTTP_GET, .handler = neo_files_get_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &neo_files_uri); if (r != ESP_OK) return r;
    httpd_uri_t neo_applets_post_uri = { .uri = "/api/v1/neo/applets", .method = HTTP_POST, .handler = neo_applets_post_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &neo_applets_post_uri); if (r != ESP_OK) return r;
    httpd_uri_t neo_applets_delete_all_uri = { .uri = "/api/v1/neo/applets", .method = HTTP_DELETE, .handler = neo_applets_delete_all_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &neo_applets_delete_all_uri); if (r != ESP_OK) return r;
    httpd_uri_t neo_applet_inspect_uri = { .uri = "/api/v1/neo/applets/inspect", .method = HTTP_POST, .handler = neo_applet_inspect_post_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &neo_applet_inspect_uri); if (r != ESP_OK) return r;
    httpd_uri_t neo_restart_uri = { .uri = "/api/v1/neo/restart", .method = HTTP_POST, .handler = neo_restart_post_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &neo_restart_uri); if (r != ESP_OK) return r;
    httpd_uri_t neo_autobackup_get_uri = { .uri = "/api/v1/neo/autobackup", .method = HTTP_GET, .handler = neo_autobackup_get_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &neo_autobackup_get_uri); if (r != ESP_OK) return r;
    httpd_uri_t neo_autobackup_post_uri = { .uri = "/api/v1/neo/autobackup", .method = HTTP_POST, .handler = neo_autobackup_post_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &neo_autobackup_post_uri); if (r != ESP_OK) return r;
    /* More-specific applet file and download routes MUST register before the
     * catch-all DELETE for a single applet id — ESP-IDF wildcard matching treats
     * the general pattern as already covering the more specific ones. */
    httpd_uri_t neo_applet_download_uri = { .uri = "/api/v1/neo/applets/*/download", .method = HTTP_GET, .handler = neo_applet_download_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &neo_applet_download_uri); if (r != ESP_OK) return r;
    httpd_uri_t neo_file_read_uri = { .uri = "/api/v1/neo/applets/*/files/*/read", .method = HTTP_POST, .handler = neo_file_read_post_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &neo_file_read_uri); if (r != ESP_OK) return r;
    httpd_uri_t neo_files_read_all_uri = { .uri = "/api/v1/neo/applets/*/files/read-all", .method = HTTP_POST, .handler = neo_files_read_all_post_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &neo_files_read_all_uri); if (r != ESP_OK) return r;
    httpd_uri_t neo_file_by_name_write_uri = { .uri = "/api/v1/neo/applets/*/files/write", .method = HTTP_POST, .handler = neo_file_by_name_write_post_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &neo_file_by_name_write_uri); if (r != ESP_OK) return r;
    httpd_uri_t neo_file_by_name_clear_uri = { .uri = "/api/v1/neo/applets/*/files", .method = HTTP_DELETE, .handler = neo_file_by_name_clear_delete_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &neo_file_by_name_clear_uri); if (r != ESP_OK) return r;
    httpd_uri_t neo_file_download_uri = { .uri = "/api/v1/neo/applets/*/files/*/download", .method = HTTP_GET, .handler = neo_file_download_get_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &neo_file_download_uri); if (r != ESP_OK) return r;
    httpd_uri_t neo_file_write_uri = { .uri = "/api/v1/neo/applets/*/files/*/write", .method = HTTP_POST, .handler = neo_file_write_post_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &neo_file_write_uri); if (r != ESP_OK) return r;
    httpd_uri_t neo_file_clear_uri = { .uri = "/api/v1/neo/applets/*/files/*", .method = HTTP_DELETE, .handler = neo_file_clear_delete_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &neo_file_clear_uri); if (r != ESP_OK) return r;
    httpd_uri_t neo_applet_delete_uri = { .uri = "/api/v1/neo/applets/*", .method = HTTP_DELETE, .handler = neo_applet_delete_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &neo_applet_delete_uri); if (r != ESP_OK) return r;

    httpd_uri_t logs_uri = { .uri = "/api/v1/logs", .method = HTTP_GET, .handler = logs_get_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &logs_uri); if (r != ESP_OK) return r;

    httpd_uri_t keyboard_clear_uri = {
        .uri = "/api/v1/keyboard/clear", .method = HTTP_POST,
        .handler = keyboard_clear_post_handler, .user_ctx = NULL
    };
    r = httpd_register_uri_handler(server, &keyboard_clear_uri);
    if (r != ESP_OK) return r;
    // Settings endpoints
    httpd_uri_t settings_get_uri = {
        .uri = "/api/v1/settings", .method = HTTP_GET,
        .handler = settings_get_handler, .user_ctx = NULL
    };
    r = httpd_register_uri_handler(server, &settings_get_uri);
    if (r != ESP_OK) return r;
    httpd_uri_t settings_post_uri = {
        .uri = "/api/v1/settings", .method = HTTP_POST,
        .handler = settings_post_handler, .user_ctx = NULL
    };
    r = httpd_register_uri_handler(server, &settings_post_uri);
    if (r != ESP_OK) return r;
    /* SD status for UI polling */
    httpd_uri_t sd_status_uri = { .uri = "/api/v1/sd/status", .method = HTTP_GET, .handler = sd_status_get_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &sd_status_uri); if (r != ESP_OK) return r;
    httpd_uri_t sd_format_uri = { .uri = "/api/v1/sd/format", .method = HTTP_POST, .handler = sd_format_post_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &sd_format_uri); if (r != ESP_OK) return r;
    // Wi-Fi endpoints
    httpd_uri_t wifi_get_uri = { .uri = "/api/v1/wifi", .method = HTTP_GET, .handler = wifi_get_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &wifi_get_uri); if (r != ESP_OK) return r;
    httpd_uri_t wifi_post_uri = { .uri = "/api/v1/wifi", .method = HTTP_POST, .handler = wifi_post_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &wifi_post_uri); if (r != ESP_OK) return r;
    httpd_uri_t wifi_confirm_uri = { .uri = "/api/v1/wifi/confirm", .method = HTTP_POST, .handler = wifi_confirm_post_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &wifi_confirm_uri); if (r != ESP_OK) return r;
    httpd_uri_t wifi_scan_uri = { .uri = "/api/v1/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &wifi_scan_uri); if (r != ESP_OK) return r;
    httpd_uri_t token_refresh_uri = { .uri = "/api/v1/token/refresh", .method = HTTP_POST, .handler = token_refresh_post_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &token_refresh_uri); if (r != ESP_OK) return r;

    httpd_uri_t sync_config_get_uri = { .uri = "/api/v1/sync/config", .method = HTTP_GET, .handler = sync_config_get_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &sync_config_get_uri); if (r != ESP_OK) return r;
    httpd_uri_t sync_config_put_uri = { .uri = "/api/v1/sync/config", .method = HTTP_PUT, .handler = sync_config_put_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &sync_config_put_uri); if (r != ESP_OK) return r;
    httpd_uri_t sync_test_uri = { .uri = "/api/v1/sync/test", .method = HTTP_POST, .handler = sync_test_post_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &sync_test_uri); if (r != ESP_OK) return r;
    httpd_uri_t sync_run_uri = { .uri = "/api/v1/sync/run", .method = HTTP_POST, .handler = sync_run_post_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &sync_run_uri); if (r != ESP_OK) return r;
    httpd_uri_t factory_reset_uri = { .uri = "/api/v1/device/factory-reset", .method = HTTP_POST, .handler = factory_reset_post_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &factory_reset_uri); if (r != ESP_OK) return r;

    register_command_endpoints(server);

    httpd_uri_t onboarding_get_uri = { .uri = "/api/v1/onboarding", .method = HTTP_GET, .handler = onboarding_get_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &onboarding_get_uri);
    if (r != ESP_OK) return r;
    httpd_uri_t onboarding_post_uri = { .uri = "/api/v1/onboarding", .method = HTTP_POST, .handler = onboarding_post_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &onboarding_post_uri);
    if (r != ESP_OK) return r;

    /* Captive + static routes must register after API routes when wildcard matching
     * is enabled; otherwise the catch-all static pattern blocks API registration. */
    httpd_uri_t captive_uri = { .uri = "/generate_204", .method = HTTP_GET, .handler = captive_check_get_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &captive_uri);
    if (r != ESP_OK) return r;
    httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = static_get_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &root_uri);
    if (r != ESP_OK) return r;
    httpd_uri_t static_uri = { .uri = "/*", .method = HTTP_GET, .handler = static_get_handler, .user_ctx = NULL };
    r = httpd_register_uri_handler(server, &static_uri);
    return r;
}

/**
 * @brief Return the recent keyboard buffer for the web UI.
 *
 * Reads the rolling buffer maintained by `neo_live` and returns it as plain
 * text. Sets an `X-Neo-Sequence` header containing the snapshot sequence.
 *
 * @param req The HTTP request to respond to.
 * @return ESP_OK after sending the text body or an error status on failure.
 */
static esp_err_t keyboard_recent_get_handler(httpd_req_t *req)
{
    char text[2048];
    unsigned long sequence = 0;
    esp_err_t result = neo_live_snapshot(text, sizeof(text), &sequence);
    if (result != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "Keyboard monitor unavailable", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "mem", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    cJSON_AddNumberToObject(root, "sequence", (double)sequence);
    cJSON_AddStringToObject(root, "text", text);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "mem", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    set_security_headers(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

/**
 * @brief Clear the recent keyboard buffer.
 *
 * POST /api/v1/keyboard/clear — requires authentication.
 *
 * @param req The HTTP request triggering the clear.
 * @return ESP_OK after sending a confirmation JSON object.
 */
static esp_err_t keyboard_clear_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) return send_unauthorized(req);
    neo_live_clear();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"cleared\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief GET /api/v1/logs?limit=N
 *
 * Returns up to `limit` recent log lines (default 32). Requires authentication.
 */
static esp_err_t logs_get_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) return send_unauthorized(req);
    char q[64];
    int limit = 32;
    log_level_t min_level = LOG_LEVEL_DEBUG;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char lims[16];
        if (httpd_query_key_value(q, "limit", lims, sizeof(lims)) == ESP_OK) {
            int v = atoi(lims);
            if (v > 0 && v <= 128) limit = v;
        }
        char mins[16];
        if (httpd_query_key_value(q, "min_level", mins, sizeof(mins)) == ESP_OK) {
            if (strcasecmp(mins, "DEBUG") == 0) min_level = LOG_LEVEL_DEBUG;
            else if (strcasecmp(mins, "INFO") == 0) min_level = LOG_LEVEL_INFO;
            else if (strcasecmp(mins, "WARN") == 0) min_level = LOG_LEVEL_WARN;
            else if (strcasecmp(mins, "ERROR") == 0) min_level = LOG_LEVEL_ERROR;
            else {
                int nv = atoi(mins);
                if (nv >= 0 && nv <= 3) min_level = (log_level_t)nv;
            }
        }
    }
    size_t out_size = (size_t)limit * 256;
    char *out = malloc(out_size);
    if (!out) { httpd_resp_set_status(req, "500 Internal Server Error"); httpd_resp_send(req, "mem", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
    memset(out, 0, out_size);
    if (log_buffer_get_recent_json(out, out_size, limit, min_level) != ESP_OK) {
        free(out);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "logs unavailable", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

/**
 * @brief GET /api/v1/settings
 *
 * Returns the persisted device settings as a JSON object. Authentication is
 * required.
 */
static esp_err_t settings_get_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) return send_unauthorized(req);
    device_settings_t s;
    if (settings_load(&s) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "error", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    settings_apply_hotspot_defaults(&s);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "mem", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    cJSON_AddStringToObject(root, "device_name", s.device_name);
    cJSON_AddStringToObject(root, "keyboard_layout", s.keyboard_layout);
    cJSON_AddNumberToObject(root, "display_brightness", s.display_brightness);
    cJSON_AddNumberToObject(root, "sleep_timeout_seconds", s.sleep_timeout_seconds);
    cJSON_AddBoolToObject(root, "onboarding_complete", s.onboarding_complete);
    cJSON_AddBoolToObject(root, "require_portal_auth", s.require_portal_auth);
    cJSON_AddBoolToObject(root, "auto_backup_on_connect", s.auto_backup_on_connect);
    cJSON_AddBoolToObject(root, "auto_cloud_sync_after_backup", s.auto_cloud_sync_after_backup);
    cJSON_AddStringToObject(root, "neo_label", s.neo_label);
    cJSON_AddStringToObject(root, "network_mode",
                            s.network_mode == SETTINGS_NETWORK_HOME ? "home" : "direct");
    cJSON_AddStringToObject(root, "hotspot_ssid", s.hotspot_ssid);
    cJSON_AddStringToObject(root, "wifi_ssid", s.wifi_ssid);
    cJSON_AddBoolToObject(root, "wifi_dhcp", s.wifi_dhcp);
    cJSON_AddStringToObject(root, "wifi_ip", s.wifi_ip);
    cJSON_AddStringToObject(root, "wifi_netmask", s.wifi_netmask);
    cJSON_AddStringToObject(root, "wifi_gateway", s.wifi_gateway);
    cJSON_AddStringToObject(root, "wifi_dns", s.wifi_dns);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "json failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

/**
 * @brief POST /api/v1/settings
 *
 * Accepts a JSON payload with optional fields and updates persistent
 * settings. Uses naive string parsing; callers should ensure valid input.
 */
static esp_err_t settings_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) return send_unauthorized(req);
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 1024) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "bad body", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char *buf = malloc(total_len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    int rlen = httpd_req_recv(req, buf, total_len);
    if (rlen <= 0) { free(buf); return ESP_FAIL; }
    buf[rlen] = '\0';
    device_settings_t s;
    settings_load(&s);
    device_settings_t prev = s;
    // Parse JSON using cJSON and update provided fields only
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        free(buf);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "invalid json", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    cJSON *item = NULL;
    item = cJSON_GetObjectItemCaseSensitive(root, "device_name");
    if (cJSON_IsString(item) && (item->valuestring != NULL)) {
        strncpy(s.device_name, item->valuestring, sizeof(s.device_name)-1);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "keyboard_layout");
    if (cJSON_IsString(item) && (item->valuestring != NULL)) {
        strncpy(s.keyboard_layout, item->valuestring, sizeof(s.keyboard_layout)-1);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "display_brightness");
    if (cJSON_IsNumber(item)) {
        s.display_brightness = (uint8_t)item->valueint;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "sleep_timeout_seconds");
    if (cJSON_IsNumber(item)) {
        s.sleep_timeout_seconds = (uint16_t)item->valueint;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "require_portal_auth");
    if (cJSON_IsBool(item)) {
        s.require_portal_auth = cJSON_IsTrue(item);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "auto_backup_on_connect");
    if (cJSON_IsBool(item)) {
        s.auto_backup_on_connect = cJSON_IsTrue(item);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "auto_cloud_sync_after_backup");
    if (cJSON_IsBool(item)) {
        s.auto_cloud_sync_after_backup = cJSON_IsTrue(item);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "neo_label");
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strncpy(s.neo_label, item->valuestring, sizeof(s.neo_label) - 1);
        s.neo_label[sizeof(s.neo_label) - 1] = '\0';
    }
    apply_network_json_fields(root, &s);
    item = cJSON_GetObjectItemCaseSensitive(root, "wifi_dhcp");
    if (cJSON_IsBool(item)) {
        s.wifi_dhcp = cJSON_IsTrue(item);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "wifi_ip");
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(s.wifi_ip, item->valuestring, sizeof(s.wifi_ip)-1);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "wifi_netmask");
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(s.wifi_netmask, item->valuestring, sizeof(s.wifi_netmask)-1);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "wifi_gateway");
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(s.wifi_gateway, item->valuestring, sizeof(s.wifi_gateway)-1);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "wifi_dns");
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(s.wifi_dns, item->valuestring, sizeof(s.wifi_dns)-1);
    }
    cJSON_Delete(root);
    free(buf);

    settings_apply_hotspot_defaults(&s);
    device_settings_t prev_norm = prev;
    settings_apply_hotspot_defaults(&prev_norm);

    bool network_changed = prev_norm.network_mode != s.network_mode
        || strcmp(prev_norm.hotspot_ssid, s.hotspot_ssid) != 0
        || strcmp(prev_norm.hotspot_password, s.hotspot_password) != 0
        || strcmp(prev_norm.wifi_ssid, s.wifi_ssid) != 0
        || strcmp(prev_norm.wifi_password, s.wifi_password) != 0
        || prev_norm.wifi_dhcp != s.wifi_dhcp
        || strcmp(prev_norm.wifi_ip, s.wifi_ip) != 0
        || strcmp(prev_norm.wifi_netmask, s.wifi_netmask) != 0
        || strcmp(prev_norm.wifi_gateway, s.wifi_gateway) != 0
        || strcmp(prev_norm.wifi_dns, s.wifi_dns) != 0;

    char err_msg[96] = {0};
    if (!settings_network_config_valid(&s, err_msg, sizeof(err_msg))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, err_msg[0] ? err_msg : "invalid network settings", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (settings_save(&s) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "error", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
#if HAVE_OLED
    display_set_brightness(s.display_brightness);
#endif
    httpd_resp_set_type(req, "application/json");
    if (network_changed && reboot_cooldown_elapsed()) {
        httpd_resp_send(req, "{\"ok\":true,\"rebooting\":true}", HTTPD_RESP_USE_STRLEN);
        schedule_device_reboot();
    } else if (network_changed) {
        ESP_LOGW("web_api", "Network changed; applying live (reboot suppressed — recent boot)");
        if (s.network_mode == SETTINGS_NETWORK_HOME) {
            wifi_manager_connect(s.wifi_ssid, s.wifi_password);
        } else {
            wifi_manager_start_ap();
        }
        httpd_resp_send(req, "{\"ok\":true,\"rebooting\":false,\"applied_live\":true}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

/* GET /api/v1/sd/status - report SD card presence and capacity for UI polling */
static esp_err_t sd_status_get_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) return send_unauthorized(req);
    device_status_t st;
    device_status_get(&st);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "alloc failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    cJSON_AddBoolToObject(root, "mounted", st.sd_card_mounted ? cJSON_True : cJSON_False);
    cJSON_AddNumberToObject(root, "size_bytes", (double)st.sd_card_total_bytes);
    cJSON_AddNumberToObject(root, "used_bytes", (double)st.sd_card_used_bytes);
    /* Formatting state is not tracked on the device yet; report false by default. */
    cJSON_AddBoolToObject(root, "formatting", cJSON_False);
    cJSON_AddNumberToObject(root, "progress", 0);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "json failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    set_security_headers(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

/* POST /api/v1/sd/format - start formatting the SD card (async) */
static esp_err_t sd_format_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) return send_unauthorized(req);
    esp_err_t r = sd_format_start();
    if (r == ESP_OK) {
        httpd_resp_set_status(req, "202 Accepted");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"started\":true}", HTTPD_RESP_USE_STRLEN);
        log_buffer_appendf("sd: format started by web UI");
        return ESP_OK;
    }
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_send(req, "format_in_progress", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Command endpoints
/**
 * @brief GET /api/v1/command/version
 *
 * Forwards a version request to the connected NEO device and returns the
 * string response as JSON. Requires authentication.
 */
static esp_err_t cmd_version_get_handler(httpd_req_t *req) {
    ESP_LOGI("web_api", "GET /command/version");
    if (!request_is_authenticated(req)) {
        ESP_LOGW("web_api", "GET /command/version denied: not authenticated");
        return send_unauthorized(req);
    }
    char buf[128];
    esp_err_t ver_err = usb_host_neo_get_version(buf, sizeof(buf));
    if (ver_err != ESP_OK) {
        ESP_LOGE("web_api", "GET /command/version failed: %s", esp_err_to_name(ver_err));
        neo_debug_event("web VERSION failed: %s", esp_err_to_name(ver_err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "error", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    ESP_LOGI("web_api", "GET /command/version ok: %s", buf);
    char out[256];
    snprintf(out, sizeof(out), "{\"version\":\"%s\"}", buf);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief GET /api/v1/wifi
 *
 * Returns Wi-Fi status JSON (state, ssid, ip). Authentication required.
 */
static esp_err_t wifi_get_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) return send_unauthorized(req);
    device_status_t st;
    device_status_get(&st);
    device_settings_t ds;
    settings_load(&ds);
    settings_apply_hotspot_defaults(&ds);
    char out[640];
    snprintf(out, sizeof(out),
             "{\"state\":%d,\"network_mode\":\"%s\",\"hotspot_ssid\":\"%s\","
             "\"ssid\":\"%s\",\"ip\":\"%s\",\"dhcp\":%s,\"static_ip\":\"%s\"}",
             (int)st.wifi_state,
             ds.network_mode == SETTINGS_NETWORK_HOME ? "home" : "direct",
             ds.hotspot_ssid, st.wifi_ssid, st.ip_address,
             (ds.wifi_dhcp ? "true" : "false"), ds.wifi_ip);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t wifi_scan_json_response(httpd_req_t *req)
{
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "scan failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    uint16_t ap_count = 32;
    wifi_ap_record_t ap_records[32];
    if (esp_wifi_scan_get_ap_records(&ap_count, ap_records) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "scan read failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    cJSON *arr = cJSON_CreateArray();
    for (uint16_t i = 0; i < ap_count; ++i) {
        if (ap_records[i].ssid[0] == '\0') {
            continue;
        }
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "ssid", (const char *)ap_records[i].ssid);
        cJSON_AddNumberToObject(it, "rssi", ap_records[i].rssi);
        cJSON_AddNumberToObject(it, "channel", ap_records[i].primary);
        cJSON_AddNumberToObject(it, "authmode", ap_records[i].authmode);
        cJSON_AddItemToArray(arr, it);
    }
    char *out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!out) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "json failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

/**
 * @brief GET /api/v1/wifi/scan
 *
 * Performs a blocking Wi‑Fi scan and returns an array of AP records as JSON.
 * Allowed without login while first-run onboarding is incomplete.
 */
static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    if (!device_allows_setup_access() && !request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    return wifi_scan_json_response(req);
}

/**
 * @brief POST /api/v1/wifi
 *
 * Accepts JSON with {"ssid":"...","password":"..."} and saves the
 * credentials then triggers a connection attempt. Returns a simple status.
 */
static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) return send_unauthorized(req);
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 1024) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "bad body", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char *buf = malloc(total_len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    int rlen = httpd_req_recv(req, buf, total_len);
    if (rlen <= 0) { free(buf); return ESP_FAIL; }
    buf[rlen] = '\0';
    char ssid[SETTINGS_WIFI_SSID_MAX_LENGTH+1] = {0};
    char password[SETTINGS_WIFI_PASSWORD_MAX_LENGTH+1] = {0};
    // Use cJSON for robust parsing
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        free(buf);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "invalid json", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    if (cJSON_IsString(item) && (item->valuestring != NULL)) {
        strncpy(ssid, item->valuestring, sizeof(ssid)-1);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (cJSON_IsString(item) && (item->valuestring != NULL)) {
        strncpy(password, item->valuestring, sizeof(password)-1);
    }
    bool dhcp = true;
    item = cJSON_GetObjectItemCaseSensitive(root, "dhcp");
    if (cJSON_IsBool(item)) dhcp = cJSON_IsTrue(item);
    char ip[64] = {0};
    char netmask[64] = {0};
    char gateway[64] = {0};
    char dns[64] = {0};
    item = cJSON_GetObjectItemCaseSensitive(root, "ip");
    if (cJSON_IsString(item) && item->valuestring) strncpy(ip, item->valuestring, sizeof(ip)-1);
    item = cJSON_GetObjectItemCaseSensitive(root, "netmask");
    if (cJSON_IsString(item) && item->valuestring) strncpy(netmask, item->valuestring, sizeof(netmask)-1);
    item = cJSON_GetObjectItemCaseSensitive(root, "gateway");
    if (cJSON_IsString(item) && item->valuestring) strncpy(gateway, item->valuestring, sizeof(gateway)-1);
    item = cJSON_GetObjectItemCaseSensitive(root, "dns");
    if (cJSON_IsString(item) && item->valuestring) strncpy(dns, item->valuestring, sizeof(dns)-1);
    cJSON_Delete(root);
    free(buf);

    if (ssid[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing ssid", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

     /* Persist credentials and network settings then attempt connect */
    device_settings_t prev;
    settings_load(&prev);
    /* prepare new settings based on previous */
    device_settings_t s = prev;
    strlcpy(s.wifi_ssid, ssid, sizeof(s.wifi_ssid));
    strlcpy(s.wifi_password, password, sizeof(s.wifi_password));
    s.network_mode = SETTINGS_NETWORK_HOME;
    s.wifi_dhcp = dhcp;
    strlcpy(s.wifi_ip, ip, sizeof(s.wifi_ip));
    strlcpy(s.wifi_netmask, netmask, sizeof(s.wifi_netmask));
    strlcpy(s.wifi_gateway, gateway, sizeof(s.wifi_gateway));
    strlcpy(s.wifi_dns, dns, sizeof(s.wifi_dns));
    settings_save(&s);

    /* Attempt station connection using provided SSID/password. If DHCP is
       disabled and a static IP was provided, attempt to apply it immediately
       — validating the IPs first to avoid leaving the device unreachable. */
    if (!s.wifi_dhcp && s.wifi_ip[0] != '\0') {
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif) {
            ip4_addr_t ip4, gw4, nm4;
            if (!ip4addr_aton(s.wifi_ip, &ip4) || !ip4addr_aton(s.wifi_gateway, &gw4) || !ip4addr_aton(s.wifi_netmask, &nm4)) {
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_send(req, "invalid ip/netmask/gateway", HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }
            esp_err_t stop = esp_netif_dhcpc_stop(sta_netif);
            (void)stop;
            esp_netif_ip_info_t ip_info;
            ip_info.ip.addr = ip4.addr;
            ip_info.gw.addr = gw4.addr;
            ip_info.netmask.addr = nm4.addr;
            if (esp_netif_set_ip_info(sta_netif, &ip_info) != ESP_OK) {
                httpd_resp_set_status(req, "500 Internal Server Error");
                httpd_resp_send(req, "set ip failed", HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }
            log_buffer_appendf("wifi: static ip applied %s", s.wifi_ip);
            /* start rollback timer: if UI doesn't confirm reachability, restore previous settings */
            if (wifi_rollback_timer) {
                esp_timer_stop(wifi_rollback_timer);
                esp_timer_delete(wifi_rollback_timer);
                wifi_rollback_timer = NULL;
            }
            memset(&wifi_prev_settings, 0, sizeof(wifi_prev_settings));
            memcpy(&wifi_prev_settings, &prev, sizeof(device_settings_t));
            wifi_rollback_pending = true;
            const esp_timer_create_args_t targs = {
                .callback = &wifi_rollback_timer_cb,
                .arg = NULL,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "wifi_rb"
            };
            if (esp_timer_create(&targs, &wifi_rollback_timer) == ESP_OK) {
                esp_timer_start_once(wifi_rollback_timer, 15000 * 1000); /* 15s */
                log_buffer_appendf("wifi: rollback timer started (15s)");
            }
            /* Apply DNS if provided */
            if (s.wifi_dns[0] != '\0') {
                ip4_addr_t dns4;
                if (ip4addr_aton(s.wifi_dns, &dns4)) {
                    esp_netif_dns_info_t dns_info;
                    dns_info.ip.u_addr.ip4.addr = dns4.addr;
                    dns_info.ip.type = IPADDR_TYPE_V4;
                    esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info);
                    log_buffer_appendf("wifi: dns applied %s", s.wifi_dns);
                } else {
                    log_buffer_appendf("wifi: invalid dns %s", s.wifi_dns);
                }
            }
        } else {
            log_buffer_appendf("wifi: STA netif unavailable; saved static IP %s", s.wifi_ip);
        }
    }
    wifi_manager_connect(s.wifi_ssid, s.wifi_password);

    /* Respond with applied state and static IP for the UI to display */
    char resp[256];
    if (!s.wifi_dhcp && s.wifi_ip[0] != '\0') {
        snprintf(resp, sizeof(resp), "{\"status\":\"connecting\",\"applied_static\":true,\"ip\":\"%s\"}", s.wifi_ip);
    } else {
        snprintf(resp, sizeof(resp), "{\"status\":\"connecting\",\"applied_static\":false}\n");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief GET /api/v1/files — list saved documents with name, size, modified time.
 */
static esp_err_t files_get_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) return send_unauthorized(req);

    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "alloc failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    file_manager_list(arr);

    char *out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!out) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "json failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    set_security_headers(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

static esp_err_t files_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) return send_unauthorized(req);

    int total_len = req->content_len;
    if (total_len <= 0 || total_len > (int)FILE_MANAGER_MAX_UPLOAD) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "invalid size", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char *body = malloc((size_t)total_len + 1);
    if (!body) return ESP_ERR_NO_MEM;
    int received = 0;
    while (received < total_len) {
        int r = httpd_req_recv(req, body + received, total_len - received);
        if (r <= 0) { free(body); return ESP_FAIL; }
        received += r;
    }
    body[total_len] = '\0';

    char filename[FILE_MANAGER_NAME_MAX + 1] = {0};
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "name", filename, sizeof(filename));
    }
    if (filename[0] == '\0') {
        httpd_req_get_hdr_value_str(req, "X-File-Name", filename, sizeof(filename));
    }

    const uint8_t *payload = (const uint8_t *)body;
    size_t payload_len = (size_t)total_len;

    cJSON *root = cJSON_Parse(body);
    if (root) {
        cJSON *name_item = cJSON_GetObjectItemCaseSensitive(root, "name");
        cJSON *content_item = cJSON_GetObjectItemCaseSensitive(root, "content");
        if (cJSON_IsString(name_item) && cJSON_IsString(content_item)) {
            strncpy(filename, name_item->valuestring, sizeof(filename) - 1);
            payload = (const uint8_t *)content_item->valuestring;
            payload_len = strlen(content_item->valuestring);
        }
        cJSON_Delete(root);
    }

    if (filename[0] == '\0') {
        free(body);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing name", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    esp_err_t err = file_manager_upload(filename, payload, payload_len);
    free(body);
    if (err == ESP_ERR_NO_MEM) {
        httpd_resp_set_status(req, "507 Insufficient Storage");
        httpd_resp_send(req, "storage full", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "upload failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t files_delete_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) return send_unauthorized(req);

    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing query", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char name[FILE_MANAGER_NAME_MAX + 1];
    char confirm[8];
    if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing name", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (httpd_query_key_value(query, "confirm", confirm, sizeof(confirm)) != ESP_OK ||
        strcmp(confirm, "true") != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "confirmation required", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (file_manager_delete(name) != ESP_OK) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "not found", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"deleted\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t files_patch_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) return send_unauthorized(req);

    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 512) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "bad body", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char *buf = malloc((size_t)total_len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    int received = 0;
    while (received < total_len) {
        int r = httpd_req_recv(req, buf + received, total_len - received);
        if (r <= 0) { free(buf); return ESP_FAIL; }
        received += r;
    }
    buf[total_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "invalid json", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    cJSON *old_item = cJSON_GetObjectItemCaseSensitive(root, "old_name");
    cJSON *new_item = cJSON_GetObjectItemCaseSensitive(root, "new_name");
    if (!cJSON_IsString(old_item) || !cJSON_IsString(new_item)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing names", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    esp_err_t err = file_manager_rename(old_item->valuestring, new_item->valuestring);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "rename failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t files_download_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) return send_unauthorized(req);

    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing query", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char name[FILE_MANAGER_NAME_MAX + 1];
    if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing name", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char path[320];
    if (file_manager_resolve_path(name, path, sizeof(path)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "bad name", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "not found", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > (long)FILE_MANAGER_MAX_UPLOAD) {
        fclose(f);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "file too large", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char *data = malloc((size_t)sz);
    if (!data) { fclose(f); return ESP_ERR_NO_MEM; }
    if (fread(data, 1, (size_t)sz, f) != (size_t)sz) {
        free(data);
        fclose(f);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "read failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    fclose(f);

    char disposition[128];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_send(req, data, (size_t)sz);
    free(data);
    return ESP_OK;
}

static esp_err_t files_view_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) return send_unauthorized(req);

    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing query", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char name[FILE_MANAGER_NAME_MAX + 1];
    if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing name", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char path[320];
    if (file_manager_resolve_path(name, path, sizeof(path)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "bad name", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "not found", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 65536) {
        fclose(f);
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_send(req, "file too large to preview", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char *data = malloc((size_t)sz + 1);
    if (!data) { fclose(f); return ESP_ERR_NO_MEM; }
    if (fread(data, 1, (size_t)sz, f) != (size_t)sz) {
        free(data);
        fclose(f);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "read failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    fclose(f);
    data[sz] = '\0';

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "content", data);
    cJSON_AddNumberToObject(root, "size", (double)sz);
    free(data);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "json failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

static esp_err_t ble_get_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) return send_unauthorized(req);
    device_status_t st;
    device_status_get(&st);
    const char *state = "idle";
    if (st.ble_state == DEVICE_BLE_PAIRING) state = "pairing";
    else if (st.ble_state == DEVICE_BLE_CONNECTED) state = "connected";

    char out[320];
    snprintf(out, sizeof(out),
             "{\"state\":\"%s\",\"advertising\":%s,\"connected\":%s,\"can_send\":%s,"
             "\"send_in_progress\":%s,\"bonded\":%d,\"passthrough\":true}",
             state,
             ble_hid_is_advertising() ? "true" : "false",
             ble_hid_is_connected() ? "true" : "false",
             ble_hid_can_send() ? "true" : "false",
             ble_hid_send_in_progress() ? "true" : "false",
             ble_hid_bonded_count());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t ble_pairing_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) return send_unauthorized(req);

    int total_len = req->content_len;
    bool enabled = true;
    if (total_len > 0 && total_len < 256) {
        char buf[256];
        int received = httpd_req_recv(req, buf, total_len);
        if (received > 0) {
            buf[received] = '\0';
            cJSON *root = cJSON_Parse(buf);
            if (root) {
                cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "enabled");
                if (cJSON_IsBool(item)) enabled = cJSON_IsTrue(item);
                cJSON_Delete(root);
            }
        }
    }
    ble_hid_set_pairing_enabled(enabled);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, enabled ? "{\"pairing\":true}" : "{\"pairing\":false}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t ble_preview_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) return send_unauthorized(req);

    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 8192) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "bad body", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char *buf = malloc((size_t)total_len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    int received = 0;
    while (received < total_len) {
        int r = httpd_req_recv(req, buf + received, total_len - received);
        if (r <= 0) { free(buf); return ESP_FAIL; }
        received += r;
    }
    buf[total_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "invalid json", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    cJSON *text_item = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (!cJSON_IsString(text_item)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing text", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char preview[256];
    size_t total = 0;
    esp_err_t err = ble_hid_preview_text(text_item->valuestring, preview, sizeof(preview), &total);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "preview failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "preview", preview);
    cJSON_AddNumberToObject(resp, "length", (double)total);
    cJSON_AddBoolToObject(resp, "can_send", ble_hid_can_send() ? 1 : 0);
    cJSON_AddBoolToObject(resp, "needs_host", ble_hid_can_send() ? 0 : 1);
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!json) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "json failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

static esp_err_t ble_send_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) return send_unauthorized(req);
    (void)req;
    esp_err_t err = ble_hid_confirm_send();
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "412 Precondition Failed");
        httpd_resp_send(req, "not connected or no preview", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "send failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"started\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t ble_cancel_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) return send_unauthorized(req);
    (void)req;
    ble_hid_cancel_send();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"cancelled\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief GET /api/v1/command/list_applets
 *
 * Requests the list of installed applets from the connected device and
 * returns the JSON-formatted result. Authentication is required.
 */
static esp_err_t cmd_list_applets_get_handler(httpd_req_t *req) {
    ESP_LOGI("web_api", "GET /command/list_applets");
    if (!request_is_authenticated(req)) {
        ESP_LOGW("web_api", "GET /command/list_applets denied: not authenticated");
        return send_unauthorized(req);
    }
    if (neo_require_connected(req) != ESP_OK) {
        return ESP_OK;
    }
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "mem", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    esp_err_t list_err = usb_host_neo_list_applets(buf, 4096);
    if (list_err != ESP_OK) {
        ESP_LOGE("web_api", "GET /command/list_applets failed: %s", esp_err_to_name(list_err));
        neo_debug_event("web LIST_APPLETS failed: %s", esp_err_to_name(list_err));
        free(buf);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "error", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    ESP_LOGI("web_api", "GET /command/list_applets ok");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}

/**
 * @brief GET /api/v1/command/file/attrs?index=N
 *
 * Fetches file attribute metadata for the Nth file on the connected device.
 * Returns 412 if no device is attached, and 500 on other failures.
 */
static esp_err_t cmd_file_attrs_get_handler(httpd_req_t *req) {
    ESP_LOGI("web_api", "GET /command/file/attrs");
    if (!request_is_authenticated(req)) {
        ESP_LOGW("web_api", "GET /command/file/attrs denied: not authenticated");
        return send_unauthorized(req);
    }
    char q[64];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing query", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char idxs[16];
    if (httpd_query_key_value(q, "index", idxs, sizeof(idxs)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing index", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    int idx = atoi(idxs);
    ESP_LOGI("web_api", "GET /command/file/attrs index=%d", idx);
    char buf[256];
    esp_err_t r = usb_host_neo_get_file_attributes((uint8_t)idx, buf, sizeof(buf));
    if (r == ESP_ERR_INVALID_STATE) {
        ESP_LOGW("web_api", "GET /command/file/attrs index=%d: neo not connected", idx);
        neo_debug_event("web GET_FILE_ATTRIBUTES index=%d: neo not connected", idx);
        httpd_resp_set_status(req, "412 Precondition Failed");
        httpd_resp_send(req, "no device", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (r == ESP_ERR_NOT_FOUND) {
        ESP_LOGI("web_api", "GET /command/file/attrs index=%d: not found", idx);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (r != ESP_OK) {
        ESP_LOGE("web_api", "GET /command/file/attrs index=%d failed: %s", idx, esp_err_to_name(r));
        neo_debug_event("web GET_FILE_ATTRIBUTES index=%d failed: %s", idx, esp_err_to_name(r));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "error", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    ESP_LOGI("web_api", "GET /command/file/attrs index=%d ok", idx);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief GET /api/v1/command/file/read?index=N
 *
 * Reads a raw file from the connected NEO device and saves it to storage
 * using `neo_import_save_raw_document`. Returns JSON describing the saved
 * path on success. Authentication required.
 */
static esp_err_t cmd_file_read_get_handler(httpd_req_t *req) {
    ESP_LOGI("web_api", "GET /command/file/read");
    if (!request_is_authenticated(req)) {
        ESP_LOGW("web_api", "GET /command/file/read denied: not authenticated");
        return send_unauthorized(req);
    }
    char q[64];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing query", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char idxs[16];
    if (httpd_query_key_value(q, "index", idxs, sizeof(idxs)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing index", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    int idx = atoi(idxs);
    ESP_LOGI("web_api", "GET /command/file/read index=%d", idx);
    // Attempt to read raw file and save to SD
    size_t buf_size = 64*1024;
    uint8_t *buf = malloc(buf_size);
    if (!buf) { httpd_resp_set_status(req, "500 Internal Server Error"); httpd_resp_send(req, "mem", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
    size_t out_len = 0;
    esp_err_t r = usb_host_neo_read_raw_file((uint8_t)idx, buf, buf_size, &out_len);
    if (r == ESP_ERR_INVALID_STATE) {
        ESP_LOGW("web_api", "GET /command/file/read index=%d: neo not connected", idx);
        neo_debug_event("web READ_RAW_FILE index=%d: neo not connected", idx);
        free(buf);
        httpd_resp_set_status(req, "412 Precondition Failed");
        httpd_resp_send(req, "no device", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (r != ESP_OK) {
        ESP_LOGE("web_api", "GET /command/file/read index=%d failed: %s", idx, esp_err_to_name(r));
        neo_debug_event("web READ_RAW_FILE index=%d failed: %s", idx, esp_err_to_name(r));
        free(buf);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "read error", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    ESP_LOGI("web_api", "GET /command/file/read index=%d read %u bytes", idx, (unsigned)out_len);
    char saved_path[256];
    // For now, use placeholder name; the real name would come from attributes
    const char *fname = "device-file";
    r = neo_import_save_raw_document(buf, out_len, fname, (uint8_t)idx, saved_path, sizeof(saved_path));
    free(buf);
    if (r != ESP_OK) {
        ESP_LOGE("web_api", "GET /command/file/read index=%d save failed: %s", idx, esp_err_to_name(r));
        neo_debug_event("web READ_RAW_FILE index=%d save failed: %s", idx, esp_err_to_name(r));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "save error", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    ESP_LOGI("web_api", "GET /command/file/read index=%d saved to %s", idx, saved_path);
    neo_debug_event("web READ_RAW_FILE index=%d saved %u bytes to %s", idx, (unsigned)out_len, saved_path);
    char out[512];
    snprintf(out, sizeof(out), "{\"saved\":true,\"path\":\"%s\"}", saved_path);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t cmd_space_available_get_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    if (neo_require_connected(req) != ESP_OK) {
        return ESP_OK;
    }
    neo_avail_space_t space = {0};
    esp_err_t err = neo_space_get_available(&space);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "space query failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char out[128];
    snprintf(out, sizeof(out), "{\"free_rom\":%lu,\"free_ram\":%lu}", (unsigned long)space.free_rom,
             (unsigned long)space.free_ram);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t cmd_space_used_get_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    if (neo_require_connected(req) != ESP_OK) {
        return ESP_OK;
    }
    char q[64];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing query", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char aid[16];
    if (httpd_query_key_value(q, "applet_id", aid, sizeof(aid)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing applet_id", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    neo_used_space_t used = {0};
    esp_err_t err = neo_space_get_used((uint16_t)atoi(aid), &used);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "space query failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char out[128];
    snprintf(out, sizeof(out), "{\"ram_used\":%lu,\"file_count\":%u}", (unsigned long)used.ram_used,
             (unsigned)used.file_count);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t cmd_settings_get_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    if (neo_require_connected(req) != ESP_OK) {
        return ESP_OK;
    }
    char q[96];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing query", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char aid[16];
    char flags[16] = "0";
    if (httpd_query_key_value(q, "applet_id", aid, sizeof(aid)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing applet_id", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_query_key_value(q, "flags", flags, sizeof(flags));
    cJSON *items = cJSON_CreateArray();
    if (!items) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "mem", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char merged[8];
    bool use_merged = httpd_query_key_value(q, "merged", merged, sizeof(merged)) == ESP_OK &&
                      (strcmp(merged, "1") == 0 || strcasecmp(merged, "true") == 0);
    esp_err_t err = use_merged ? neo_settings_get_merged_json((uint16_t)atoi(aid), items)
                               : neo_settings_get_json((uint16_t)atoi(aid), (uint32_t)strtoul(flags, NULL, 0), items);
    if (err != ESP_OK) {
        cJSON_Delete(items);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "settings read failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char *out = cJSON_PrintUnformatted(items);
    cJSON_Delete(items);
    if (!out) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "mem", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

static esp_err_t cmd_settings_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    if (neo_require_connected(req) != ESP_OK) {
        return ESP_OK;
    }
    char q[64];
    unsigned int applet_id = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char aid[16];
        if (httpd_query_key_value(q, "applet_id", aid, sizeof(aid)) == ESP_OK) {
            applet_id = (unsigned int)atoi(aid);
        }
    }
    uint8_t *body = NULL;
    size_t body_len = 0;
    esp_err_t err = neo_read_request_body(req, &body, &body_len);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing body", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    cJSON *root = cJSON_ParseWithLength((const char *)body, body_len);
    free(body);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "invalid json", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    cJSON *items = cJSON_GetObjectItem(root, "items");
    if (!items) {
        items = root;
    }
    err = neo_settings_set_json((uint16_t)applet_id, items);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "settings write failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t cmd_info_get_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    if (neo_require_connected(req) != ESP_OK) {
        return ESP_OK;
    }
    cJSON *info = cJSON_CreateObject();
    if (!info) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "mem", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    esp_err_t err = usb_host_neo_get_system_info(info);
    if (err != ESP_OK) {
        cJSON_Delete(info);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "info query failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char *out = cJSON_PrintUnformatted(info);
    cJSON_Delete(info);
    if (!out) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "mem", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

static esp_err_t cmd_mode_get_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    if (!usb_host_neo_is_connected()) {
        httpd_resp_set_status(req, "412 Precondition Failed");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"neo_not_connected\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char out[64];
    snprintf(out, sizeof(out), "{\"mode\":\"%s\"}", usb_host_neo_get_mode());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t cmd_settings_by_ident_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return send_unauthorized(req);
    }
    if (neo_require_connected(req) != ESP_OK) {
        return ESP_OK;
    }
    char q[64];
    unsigned int applet_id = 0;
    unsigned int ident = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char aid[16];
        char id_str[16];
        if (httpd_query_key_value(q, "applet_id", aid, sizeof(aid)) == ESP_OK) {
            applet_id = (unsigned int)atoi(aid);
        }
        if (httpd_query_key_value(q, "ident", id_str, sizeof(id_str)) == ESP_OK) {
            ident = (unsigned int)atoi(id_str);
        }
    }
    if (ident == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing ident", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    uint8_t *body = NULL;
    size_t body_len = 0;
    esp_err_t err = neo_read_request_body(req, &body, &body_len);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing body", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    cJSON *values = cJSON_ParseWithLength((const char *)body, body_len);
    free(body);
    if (!values || !cJSON_IsArray(values)) {
        cJSON_Delete(values);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "expected JSON array", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    err = neo_settings_set_by_ident((uint16_t)applet_id, (uint16_t)ident, values);
    cJSON_Delete(values);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, err == ESP_ERR_NOT_FOUND ? "404 Not Found" : "500 Internal Server Error");
        httpd_resp_send(req, "settings write failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* POST /api/v1/wifi/confirm — called by the web UI after it has successfully
   connected to the device at the new static IP to cancel the rollback timer. */
static esp_err_t wifi_confirm_post_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) return send_unauthorized(req);
    if (wifi_rollback_pending) {
        if (wifi_rollback_timer) {
            esp_timer_stop(wifi_rollback_timer);
            esp_timer_delete(wifi_rollback_timer);
            wifi_rollback_timer = NULL;
        }
        wifi_rollback_pending = false;
        log_buffer_appendf("wifi: rollback canceled by UI confirmation");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Register command endpoints
static void register_command_endpoints(httpd_handle_t server) {
    httpd_uri_t uri = {
        .uri = "/api/v1/command/version",
        .method = HTTP_GET,
        .handler = cmd_version_get_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri));

    httpd_uri_t uri2 = {
        .uri = "/api/v1/command/list_applets",
        .method = HTTP_GET,
        .handler = cmd_list_applets_get_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri2));

    httpd_uri_t uri3 = {
        .uri = "/api/v1/command/file/attrs",
        .method = HTTP_GET,
        .handler = cmd_file_attrs_get_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri3));

    httpd_uri_t uri4 = {
        .uri = "/api/v1/command/file/read",
        .method = HTTP_GET,
        .handler = cmd_file_read_get_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri4));

    httpd_uri_t uri5 = {
        .uri = "/api/v1/command/space/available",
        .method = HTTP_GET,
        .handler = cmd_space_available_get_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri5));

    httpd_uri_t uri6 = {
        .uri = "/api/v1/command/space/used",
        .method = HTTP_GET,
        .handler = cmd_space_used_get_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri6));

    httpd_uri_t uri7 = {
        .uri = "/api/v1/command/settings",
        .method = HTTP_GET,
        .handler = cmd_settings_get_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri7));

    httpd_uri_t uri8 = {
        .uri = "/api/v1/command/settings",
        .method = HTTP_POST,
        .handler = cmd_settings_post_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri8));

    httpd_uri_t uri9 = {
        .uri = "/api/v1/command/info",
        .method = HTTP_GET,
        .handler = cmd_info_get_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri9));

    httpd_uri_t uri10 = {
        .uri = "/api/v1/command/mode",
        .method = HTTP_GET,
        .handler = cmd_mode_get_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri10));

    httpd_uri_t uri11 = {
        .uri = "/api/v1/command/settings/by-ident",
        .method = HTTP_POST,
        .handler = cmd_settings_by_ident_post_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri11));
}

/* Serve static files from the SPIFFS mount at /spiflash. Files are streamed in
   small chunks so large assets (e.g. app.js) do not require a full-file heap copy. */
static const char *spiffs_base = "/spiflash";
#define STATIC_FILE_CHUNK_SIZE 2048

static const char *guess_content_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    ext++;
    if (strcasecmp(ext, "html") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(ext, "css") == 0) return "text/css";
    if (strcasecmp(ext, "js") == 0) return "application/javascript";
    if (strcasecmp(ext, "json") == 0) return "application/json";
    if (strcasecmp(ext, "png") == 0) return "image/png";
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, "svg") == 0) return "image/svg+xml";
    if (strcasecmp(ext, "txt") == 0) return "text/plain";
    return "application/octet-stream";
}

static esp_err_t serve_static_file(httpd_req_t *req, const char *fs_path)
{
    struct stat st;
    if (stat(fs_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "Not found", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    FILE *f = fopen(fs_path, "rb");
    if (!f) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "read error", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, guess_content_type(fs_path));

    char chunk[STATIC_FILE_CHUNK_SIZE];
    size_t read_bytes = 0;
    esp_err_t err = ESP_OK;
    while ((read_bytes = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
    }
    if (ferror(f)) {
        err = ESP_FAIL;
    }
    fclose(f);

    if (err != ESP_OK) {
        return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t static_get_handler(httpd_req_t *req)
{
    const char *uri = req->uri;
    /* Optionally require portal auth before serving SPA assets */
    device_settings_t ds;
    if (settings_load(&ds) == ESP_OK && ds.require_portal_auth) {
        /* If client is not authenticated, only allow login endpoints and static assets needed for login. */
        if (!request_is_authenticated(req)) {
            /* Allow login POST and token refresh but block everything else under / */
            if (strcmp(uri, "/api/v1/login") == 0 || strcmp(uri, "/api/v1/token/refresh") == 0 || strstr(uri, ".css") || strstr(uri, ".js")) {
                // allow
            } else {
                httpd_resp_set_status(req, "401 Unauthorized");
                httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }
        }
    }
    char path[1280];
    size_t uri_len = strlen(uri);
    /* Captive-portal probes can exceed static asset path lengths; serve the SPA home. */
    if (uri_len + strlen(spiffs_base) + 16 >= sizeof(path)) {
        snprintf(path, sizeof(path), "%s/index.html", spiffs_base);
        return serve_static_file(req, path);
    }
    /* Handle common captive detection paths specially */
    if (strcmp(uri, "/generate_204") == 0) {
        /* Android expects 204; this route is registered separately but
           accept here as well. */
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    if (strcmp(uri, "/hotspot-detect.html") == 0 || strcmp(uri, "/hotspot-login.html") == 0) {
        if (device_allows_setup_access()) {
            return redirect_to_setup(req);
        }
        snprintf(path, sizeof(path), "%s/index.html", spiffs_base);
        return serve_static_file(req, path);
    }
    if (strcmp(uri, "/ncsi.txt") == 0 || strcmp(uri, "/connecttest.txt") == 0) {
        /* Windows NCSI / connectivity probes */
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Microsoft NCSI", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (strcmp(uri, "/favicon.ico") == 0) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    /* map root to setup (first run) or index.html */
    if (strcmp(uri, "/") == 0 || strcmp(uri, "") == 0) {
        if (device_allows_setup_access()) {
            return redirect_to_setup(req);
        }
        snprintf(path, sizeof(path), "%s/index.html", spiffs_base);
        return serve_static_file(req, path);
    }
    /* prevent path traversal */
    if (strstr(uri, "..")) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "bad path", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    snprintf(path, sizeof(path), "%s%s", spiffs_base, uri);
    /* If uri ends with '/', serve index.html under that path */
    if (uri[strlen(uri)-1] == '/') {
        strncat(path, "index.html", sizeof(path) - strlen(path) - 1);
    }
    return serve_static_file(req, path);
}

/* Minimal captive portal check: Android expects 204 at /generate_204. */
static esp_err_t captive_check_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}
