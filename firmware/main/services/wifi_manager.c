/**
 * @file wifi_manager.c
 * @brief Wi-Fi manager for Direct access (AP) and Home network (STA) modes.
 *
 * Home network uses STA-only to avoid AP+STA brownouts during authentication.
 * Recovery AP transitions are deferred to a worker task — never from Wi-Fi events.
 */

#include "wifi_manager.h"
#include "settings.h"
#include "device_status.h"
#include "captive_dns.h"
#include "display.h"
#include "log_buffer.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "wifi_manager";
static bool g_connected = false;
static bool g_wifi_started = false;
static bool g_recovery_mode = false;
static int g_sta_fail_count = 0;
static esp_timer_handle_t g_recovery_timer = NULL;
static TaskHandle_t g_recovery_worker = NULL;

#define WIFI_RECOVERY_FAIL_THRESHOLD 2
#define WIFI_RECOVERY_TIMEOUT_US (45 * 1000 * 1000LL)
/** 8 dBm (units of 0.25 dBm) — lowers peak current during STA auth on USB power. */
#define WIFI_CONNECT_TX_POWER_QDBM 32

static void wifi_manager_start_sntp(void)
{
    static bool started = false;
    if (started) {
        return;
    }
    setenv("TZ", "UTC0", 1);
    tzset();
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.start = true;
    config.wait_for_sync = false;
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        started = true;
        ESP_LOGI(TAG, "SNTP started (pool.ntp.org)");
        log_buffer_appendf("wifi: sntp started");
    } else {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
    }
}

static void wifi_manager_sanitize_hostname(const char *name, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    size_t o = 0;
    for (size_t i = 0; name && name[i] != '\0' && o + 1 < out_size; i++) {
        unsigned char c = (unsigned char)name[i];
        if (isalnum(c)) {
            out[o++] = (char)tolower(c);
        } else if ((c == ' ' || c == '_' || c == '-') && o > 0 && out[o - 1] != '-') {
            out[o++] = '-';
        }
    }
    while (o > 0 && out[o - 1] == '-') {
        o--;
    }
    out[o] = '\0';
    if (o == 0) {
        strlcpy(out, "neo2buddy", out_size);
    }
}

static void wifi_manager_start_mdns(void)
{
    static bool started = false;
    char hostname[48];
    wifi_manager_sanitize_hostname(settings_get_device_name(), hostname, sizeof(hostname));

    if (!started) {
        esp_err_t err = mdns_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
            return;
        }
        started = true;
    }
    (void)mdns_hostname_set(hostname);
    (void)mdns_instance_name_set(settings_get_device_name());
    /* remove may fail if never registered — ignore and (re)add */
    (void)mdns_service_remove("_http", "_tcp");
    esp_err_t add = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (add != ESP_OK && add != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mDNS _http service add failed: %s", esp_err_to_name(add));
    }
    ESP_LOGI(TAG, "mDNS hostname %s.local", hostname);
    log_buffer_appendf("wifi: mdns %s.local", hostname);
}

static bool disconnect_reason_triggers_recovery(int reason)
{
    return reason == WIFI_REASON_AUTH_EXPIRE ||
           reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_MIC_FAILURE ||
           reason == WIFI_REASON_AUTH_FAIL ||
           reason == WIFI_REASON_NO_AP_FOUND ||
           reason == WIFI_REASON_ASSOC_FAIL ||
           reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
}

static void cancel_recovery_timer(void)
{
    if (!g_recovery_timer) {
        return;
    }
    esp_timer_stop(g_recovery_timer);
    esp_timer_delete(g_recovery_timer);
    g_recovery_timer = NULL;
}

static void sanitize_ap_password(wifi_config_t *ap_config)
{
    size_t len = strnlen((char *)ap_config->ap.password, sizeof(ap_config->ap.password));
    if (len < 8) {
        strlcpy((char *)ap_config->ap.password, "neo2buddy", sizeof(ap_config->ap.password));
        ap_config->ap.authmode = WIFI_AUTH_WPA2_PSK;
    }
}

static void fill_ap_config_from_settings(wifi_config_t *ap_config)
{
    device_settings_t s;
    settings_load(&s);
    settings_apply_hotspot_defaults(&s);

    memset(ap_config, 0, sizeof(*ap_config));
    strlcpy((char *)ap_config->ap.ssid, s.hotspot_ssid, sizeof(ap_config->ap.ssid));
    ap_config->ap.ssid_len = 0;
    ap_config->ap.max_connection = 4;
    ap_config->ap.authmode = WIFI_AUTH_WPA2_PSK;
    strlcpy((char *)ap_config->ap.password, s.hotspot_password, sizeof(ap_config->ap.password));
    sanitize_ap_password(ap_config);
}

static void configure_ap_netif(void)
{
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap_netif) {
        return;
    }
    ip4_addr_t ip4, gw4, nm4;
    if (ip4addr_aton("192.168.4.1", &ip4) && ip4addr_aton("192.168.4.1", &gw4) && ip4addr_aton("255.255.255.0", &nm4)) {
        esp_netif_ip_info_t ip_info;
        ip_info.ip.addr = ip4.addr;
        ip_info.gw.addr = gw4.addr;
        ip_info.netmask.addr = nm4.addr;
        esp_netif_set_ip_info(ap_netif, &ip_info);
    }
}

static void wifi_manager_stop_radio(void)
{
    cancel_recovery_timer();
    if (!g_wifi_started) {
        return;
    }
    captive_dns_stop();
    esp_wifi_stop();
    g_wifi_started = false;
    g_connected = false;
    vTaskDelay(pdMS_TO_TICKS(150));
}

static void wifi_apply_connect_power_profile(void)
{
    esp_err_t err = esp_wifi_set_max_tx_power(WIFI_CONNECT_TX_POWER_QDBM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_max_tx_power failed: %s", esp_err_to_name(err));
    }
}

static esp_err_t start_recovery_ap_now(void)
{
    if (g_recovery_mode) {
        return ESP_OK;
    }

    g_recovery_mode = true;
    g_sta_fail_count = 0;
    cancel_recovery_timer();

    wifi_config_t ap_config;
    fill_ap_config_from_settings(&ap_config);

    wifi_manager_stop_radio();

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode(AP) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP config failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start(recovery AP) failed: %s", esp_err_to_name(err));
        return err;
    }
    g_wifi_started = true;
    wifi_apply_connect_power_profile();

    device_status_set_wifi(DEVICE_WIFI_UNCONFIGURED, (const char *)ap_config.ap.ssid, "");
    ESP_LOGW(TAG, "Recovery hotspot ssid=%s — open http://192.168.4.1/setup.html", ap_config.ap.ssid);
    log_buffer_appendf("wifi: recovery AP ssid=%s", ap_config.ap.ssid);
    configure_ap_netif();
    captive_dns_start();
    display_show_onboarding((const char *)ap_config.ap.ssid, (const char *)ap_config.ap.password,
                            "Home Wi-Fi failed — fix at setup");
    return ESP_OK;
}

static void recovery_worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (g_recovery_mode) {
            continue;
        }
        start_recovery_ap_now();
    }
}

static void ensure_recovery_worker(void)
{
    if (g_recovery_worker) {
        return;
    }
    if (xTaskCreate(recovery_worker_task, "wifi_rec", 4096, NULL, 6, &g_recovery_worker) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create recovery worker task");
        g_recovery_worker = NULL;
    }
}

static void schedule_recovery_ap(void)
{
    if (g_recovery_mode) {
        return;
    }
    ensure_recovery_worker();
    if (g_recovery_worker) {
        xTaskNotifyGive(g_recovery_worker);
    } else {
        ESP_LOGW(TAG, "Recovery worker unavailable — deferring via timer");
    }
}

static void start_recovery_timer(void);

static void recovery_timer_cb(void *arg)
{
    (void)arg;
    if (g_connected || g_recovery_mode || !g_wifi_started) {
        return;
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK || mode != WIFI_MODE_STA) {
        return;
    }
    ESP_LOGW(TAG, "Home Wi-Fi timeout — scheduling recovery hotspot");
    schedule_recovery_ap();
}

static void start_recovery_timer(void)
{
    cancel_recovery_timer();
    const esp_timer_create_args_t args = {
        .callback = recovery_timer_cb,
        .name = "wifi_recovery",
    };
    if (esp_timer_create(&args, &g_recovery_timer) == ESP_OK) {
        esp_timer_start_once(g_recovery_timer, WIFI_RECOVERY_TIMEOUT_US);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
            device_status_set_wifi(DEVICE_WIFI_CONNECTING, "", "");
        } else if (event_id == WIFI_EVENT_AP_START) {
            device_status_set_wifi(DEVICE_WIFI_UNCONFIGURED, "", "");
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            if (g_recovery_mode) {
                return;
            }
            wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
            g_connected = false;
            device_status_set_wifi(DEVICE_WIFI_ERROR, "", "");
            int reason = disc ? (int)disc->reason : -1;
            ESP_LOGW(TAG, "Station disconnected (reason=%d)", reason);
            log_buffer_appendf("wifi: STA disconnected reason=%d", reason);
            if (disconnect_reason_triggers_recovery(reason)) {
                g_sta_fail_count++;
                ESP_LOGW(TAG, "Home join failure %d/%d (reason=%d)",
                         g_sta_fail_count, WIFI_RECOVERY_FAIL_THRESHOLD, reason);
            }
            if (g_sta_fail_count >= WIFI_RECOVERY_FAIL_THRESHOLD) {
                ESP_LOGW(TAG, "Scheduling recovery hotspot — join it to fix home Wi-Fi");
                schedule_recovery_ap();
                return;
            }
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = event_data;
        g_connected = true;
        g_sta_fail_count = 0;
        g_recovery_mode = false;
        cancel_recovery_timer();
        char ipstr[32];
        esp_ip4addr_ntoa(&ev->ip_info.ip, ipstr, sizeof(ipstr));
        device_status_set_wifi(DEVICE_WIFI_CONNECTED, settings_get_wifi_ssid(), ipstr);
        display_request_home();
        wifi_manager_start_sntp();
        wifi_manager_start_mdns();
        ESP_LOGI(TAG, "Home network connected. IP=%s (portal: http://%s/)", ipstr, ipstr);
        log_buffer_appendf("wifi: connected ip=%s", ipstr);
    }
}

esp_err_t wifi_manager_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WIFI event register failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IP event register failed: %s", esp_err_to_name(err));
        return err;
    }

    ensure_recovery_worker();

    device_settings_t s;
    settings_load(&s);

    if (!s.onboarding_complete) {
        ESP_LOGI(TAG, "First boot — starting Direct access hotspot");
        return wifi_manager_start_ap();
    }

    if (s.network_mode == SETTINGS_NETWORK_HOME && s.wifi_ssid[0] != '\0') {
        ESP_LOGI(TAG, "Home network mode — joining SSID=%s", s.wifi_ssid);
        err = wifi_manager_connect(s.wifi_ssid, s.wifi_password);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Home Wi-Fi failed (%s); falling back to Direct access", esp_err_to_name(err));
            return wifi_manager_start_ap();
        }
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Direct access mode — starting device hotspot");
    return wifi_manager_start_ap();
}

esp_err_t wifi_manager_start_recovery_ap(void)
{
    schedule_recovery_ap();
    return ESP_OK;
}

esp_err_t wifi_manager_force_recovery_ap(void)
{
    return start_recovery_ap_now();
}

esp_err_t wifi_manager_start_ap(void)
{
    g_recovery_mode = false;
    g_sta_fail_count = 0;

    wifi_config_t ap_config;
    fill_ap_config_from_settings(&ap_config);

    wifi_manager_stop_radio();

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode(AP) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP config failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start(AP) failed: %s", esp_err_to_name(err));
        return err;
    }
    g_wifi_started = true;
    wifi_apply_connect_power_profile();

    device_status_set_wifi(DEVICE_WIFI_UNCONFIGURED, (const char *)ap_config.ap.ssid, "");
    ESP_LOGI(TAG, "Direct access hotspot started ssid=%s ip=192.168.4.1", ap_config.ap.ssid);
    log_buffer_appendf("wifi: direct access ssid=%s", ap_config.ap.ssid);
    configure_ap_netif();
    captive_dns_start();
    wifi_manager_start_mdns();
    display_show_onboarding((const char *)ap_config.ap.ssid, (const char *)ap_config.ap.password,
                            "http://192.168.4.1/setup.html");
    return ESP_OK;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    if (password) {
        strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);
    }

    wifi_manager_stop_radio();

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode(STA) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STA config failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start(STA) failed: %s", esp_err_to_name(err));
        return err;
    }
    g_wifi_started = true;
    wifi_apply_connect_power_profile();

    g_recovery_mode = false;
    g_sta_fail_count = 0;
    start_recovery_timer();

    device_status_set_wifi(DEVICE_WIFI_CONNECTING, ssid, "");
    ESP_LOGI(TAG, "Connecting to home network SSID=%s (STA-only)", ssid);
    log_buffer_appendf("wifi: connecting to %s", ssid);
    display_show_onboarding(ssid, "", "Connecting to home network...");
    return ESP_OK;
}

esp_err_t wifi_manager_stop(void)
{
    wifi_manager_stop_radio();
    device_status_set_wifi(DEVICE_WIFI_UNCONFIGURED, "", "");
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return g_connected;
}

bool wifi_manager_is_recovery_mode(void)
{
    return g_recovery_mode;
}
