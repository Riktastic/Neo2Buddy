/**
 * @file display.c
 * @brief SSD1306 OLED driver (128×64, I2C) for onboarding and home screen.
 *
 * Page-buffer rendering with a built-in 5×7 font — no external graphics lib.
 * Shows AP SSID/password during setup, then IP + battery + backup count on the
 * home screen. In Neo USB keyboard mode, the bottom line shows live typing.
 * Pins and HAVE_OLED come from board_config.h. Probes 0x3C then 0x3D.
 */

#include "display.h"
#include "log_buffer.h"
#include "device_status.h"
#include "settings.h"
#include "file_manager.h"
#include "neo_live.h"
#include "usb_host_neo.h"
#include "cJSON.h"
#include "esp_log.h"
#include "board_config.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "display";

/* SSD1306 I2C address (7-bit); some modules use 0x3D. */
#define SSD1306_I2C_ADDR_PRIMARY 0x3C
#define SSD1306_I2C_ADDR_ALT     0x3D

/* Display geometry */
#define SSD1306_WIDTH 128
#define SSD1306_HEIGHT 64
#define SSD1306_PAGES (SSD1306_HEIGHT/8)

/* Forward declarations */
static esp_err_t ssd1306_i2c_init(void);
static esp_err_t ssd1306_probe(void);
static esp_err_t ssd1306_write_cmd(uint8_t cmd);
static esp_err_t ssd1306_write_data(const uint8_t *data, size_t len);
static void ssd1306_init_display(void);
static esp_err_t ssd1306_update_page(uint8_t page, const uint8_t *buf128);

/* One page (128 bytes) buffer used for page-buffer rendering */
static uint8_t s_pagebuf[SSD1306_WIDTH];
static bool s_inited = false;
static bool s_oled_absent = false;
static uint8_t s_i2c_addr = SSD1306_I2C_ADDR_PRIMARY;
typedef enum {
    DISPLAY_MODE_HOME,
    DISPLAY_MODE_ONBOARDING,
} display_mode_t;

static display_mode_t s_mode = DISPLAY_MODE_HOME;
static TaskHandle_t s_refresh_task = NULL;

static void display_refresh_task(void *arg);

static void ensure_refresh_task(void)
{
    if (s_refresh_task != NULL) {
        return;
    }
    if (xTaskCreate(display_refresh_task, "display", 6144, NULL, 2, &s_refresh_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create display refresh task");
        s_refresh_task = NULL;
    }
}

/* Basic 5x7 font for ASCII 32..127 (96 chars * 5 bytes). Each byte encodes
 * a column of 8 vertical pixels (LSB at top). */
static const uint8_t font5x7[96][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* 32 ' ' */
    {0x00,0x00,0x5F,0x00,0x00}, /* 33 ! */
    {0x00,0x07,0x00,0x07,0x00}, /* 34 " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* 35 # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* 36 $ */
    {0x23,0x13,0x08,0x64,0x62}, /* 37 % */
    {0x36,0x49,0x55,0x22,0x50}, /* 38 & */
    {0x00,0x05,0x03,0x00,0x00}, /* 39 ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* 40 ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* 41 ) */
    {0x14,0x08,0x3E,0x08,0x14}, /* 42 * */
    {0x08,0x08,0x3E,0x08,0x08}, /* 43 + */
    {0x00,0x50,0x30,0x00,0x00}, /* 44 , */
    {0x08,0x08,0x08,0x08,0x08}, /* 45 - */
    {0x00,0x60,0x60,0x00,0x00}, /* 46 . */
    {0x20,0x10,0x08,0x04,0x02}, /* 47 / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 48 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 49 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 50 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 51 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 52 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 53 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 54 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 55 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 56 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 57 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* 58 : */
    {0x00,0x56,0x36,0x00,0x00}, /* 59 ; */
    {0x08,0x14,0x22,0x41,0x00}, /* 60 < */
    {0x14,0x14,0x14,0x14,0x14}, /* 61 = */
    {0x00,0x41,0x22,0x14,0x08}, /* 62 > */
    {0x02,0x01,0x51,0x09,0x06}, /* 63 ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* 64 @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 65 A */
    {0x7F,0x49,0x49,0x49,0x36}, /* 66 B */
    {0x3E,0x41,0x41,0x41,0x22}, /* 67 C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 68 D */
    {0x7F,0x49,0x49,0x49,0x41}, /* 69 E */
    {0x7F,0x09,0x09,0x09,0x01}, /* 70 F */
    {0x3E,0x41,0x49,0x49,0x7A}, /* 71 G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 72 H */
    {0x00,0x41,0x7F,0x41,0x00}, /* 73 I */
    {0x20,0x40,0x41,0x3F,0x01}, /* 74 J */
    {0x7F,0x08,0x14,0x22,0x41}, /* 75 K */
    {0x7F,0x40,0x40,0x40,0x40}, /* 76 L */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* 77 M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 78 N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 79 O */
    {0x7F,0x09,0x09,0x09,0x06}, /* 80 P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 81 Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* 82 R */
    {0x46,0x49,0x49,0x49,0x31}, /* 83 S */
    {0x01,0x01,0x7F,0x01,0x01}, /* 84 T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 85 U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 86 V */
    {0x7F,0x20,0x18,0x20,0x7F}, /* 87 W */
    {0x63,0x14,0x08,0x14,0x63}, /* 88 X */
    {0x03,0x04,0x78,0x04,0x03}, /* 89 Y */
    {0x61,0x51,0x49,0x45,0x43}, /* 90 Z */
    {0x00,0x7F,0x41,0x41,0x00}, /* 91 [ */
    {0x02,0x04,0x08,0x10,0x20}, /* 92 \ */
    {0x00,0x41,0x41,0x7F,0x00}, /* 93 ] */
    {0x04,0x02,0x01,0x02,0x04}, /* 94 ^ */
    {0x40,0x40,0x40,0x40,0x40}, /* 95 _ */
    {0x00,0x01,0x02,0x04,0x00}, /* 96 ` */
    {0x20,0x54,0x54,0x54,0x78}, /* 97 a */
    {0x7F,0x48,0x44,0x44,0x38}, /* 98 b */
    {0x38,0x44,0x44,0x44,0x20}, /* 99 c */
    {0x38,0x44,0x44,0x48,0x7F}, /*100 d */
    {0x38,0x54,0x54,0x54,0x18}, /*101 e */
    {0x08,0x7E,0x09,0x01,0x02}, /*102 f */
    {0x0C,0x52,0x52,0x52,0x3E}, /*103 g */
    {0x7F,0x08,0x04,0x04,0x78}, /*104 h */
    {0x00,0x44,0x7D,0x40,0x00}, /*105 i */
    {0x20,0x40,0x44,0x3D,0x00}, /*106 j */
    {0x7F,0x10,0x28,0x44,0x00}, /*107 k */
    {0x00,0x41,0x7F,0x40,0x00}, /*108 l */
    {0x7C,0x04,0x18,0x04,0x78}, /*109 m */
    {0x7C,0x08,0x04,0x04,0x78}, /*110 n */
    {0x38,0x44,0x44,0x44,0x38}, /*111 o */
    {0x7C,0x14,0x14,0x14,0x08}, /*112 p */
    {0x08,0x14,0x14,0x18,0x7C}, /*113 q */
    {0x7C,0x08,0x04,0x04,0x08}, /*114 r */
    {0x48,0x54,0x54,0x54,0x20}, /*115 s */
    {0x04,0x3F,0x44,0x40,0x20}, /*116 t */
    {0x3C,0x40,0x40,0x20,0x7C}, /*117 u */
    {0x1C,0x20,0x40,0x20,0x1C}, /*118 v */
    {0x3C,0x40,0x30,0x40,0x3C}, /*119 w */
    {0x44,0x28,0x10,0x28,0x44}, /*120 x */
    {0x0C,0x50,0x50,0x50,0x3C}, /*121 y */
    {0x44,0x64,0x54,0x4C,0x44}, /*122 z */
    {0x00,0x08,0x36,0x41,0x00}, /*123 { */
    {0x00,0x00,0x7F,0x00,0x00}, /*124 | */
    {0x00,0x41,0x36,0x08,0x00}, /*125 } */
    {0x10,0x08,0x08,0x10,0x08}, /*126 ~ */
    {0x00,0x00,0x00,0x00,0x00}  /*127 DEL (unused) */
};

static esp_err_t ssd1306_i2c_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    esp_err_t err = i2c_driver_install(I2C_NUM_0, conf.mode, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    return i2c_param_config(I2C_NUM_0, &conf);
}

static esp_err_t ssd1306_probe_addr(uint8_t addr)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(200));
    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t ssd1306_probe(void)
{
    static const uint8_t addrs[] = { SSD1306_I2C_ADDR_PRIMARY, SSD1306_I2C_ADDR_ALT };
    for (size_t i = 0; i < sizeof(addrs); ++i) {
        if (ssd1306_probe_addr(addrs[i]) == ESP_OK) {
            s_i2c_addr = addrs[i];
            ESP_LOGI(TAG, "SSD1306 found at I2C 0x%02X (SDA=GPIO%d SCL=GPIO%d)",
                     s_i2c_addr, (int)BOARD_I2C_SDA_GPIO, (int)BOARD_I2C_SCL_GPIO);
            return ESP_OK;
        }
    }
    ESP_LOGE(TAG, "No SSD1306 on I2C bus (SDA=GPIO%d SCL=GPIO%d). "
             "Wire OLED to pUEXT or GPIO47/48, 3.3V, GND.",
             (int)BOARD_I2C_SDA_GPIO, (int)BOARD_I2C_SCL_GPIO);
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t ssd1306_write_cmd(uint8_t cmd)
{
    uint8_t data[2] = {0x00, cmd};
    return i2c_master_write_to_device(I2C_NUM_0, s_i2c_addr, data, sizeof(data), pdMS_TO_TICKS(1000));
}

static esp_err_t ssd1306_write_data(const uint8_t *data, size_t len)
{
    /* Prepend data-control byte 0x40 */
    uint8_t buf[SSD1306_WIDTH + 1];
    if (len > SSD1306_WIDTH) len = SSD1306_WIDTH;
    buf[0] = 0x40;
    memcpy(&buf[1], data, len);
    return i2c_master_write_to_device(I2C_NUM_0, s_i2c_addr, buf, len + 1, pdMS_TO_TICKS(1000));
}

static void ssd1306_init_display(void)
{
    const uint8_t init_seq[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
        0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6,
        0xAF,
    };
    for (size_t i = 0; i < sizeof(init_seq); ++i) {
        if (ssd1306_write_cmd(init_seq[i]) != ESP_OK) {
            ESP_LOGE(TAG, "SSD1306 init command failed");
            return;
        }
    }
}

static esp_err_t ssd1306_update_page(uint8_t page, const uint8_t *buf128)
{
    esp_err_t err = ssd1306_write_cmd(0xB0 | (page & 0x07));
    if (err != ESP_OK) return err;
    err = ssd1306_write_cmd(0x00);
    if (err != ESP_OK) return err;
    err = ssd1306_write_cmd(0x10);
    if (err != ESP_OK) return err;
    return ssd1306_write_data(buf128, SSD1306_WIDTH);
}

/* Render a single ASCII string into a page buffer at column 0. Each char
 * uses 6 columns (5 font + 1 spacer). If the text is longer than the
 * display width it will be truncated. This simple routine places the font
 * LSB at the top of the page (y offset 0 within page). */
static void render_text_to_pagebuf(const char *s, uint8_t *pagebuf)
{
    memset(pagebuf, 0x00, SSD1306_WIDTH);
    int col = 0;
    while (*s && col < (SSD1306_WIDTH - 6)) {
        unsigned char c = (unsigned char)*s;
        if (c < 32 || c > 126) c = '?';
        const uint8_t *g = font5x7[c - 32];
        for (int i = 0; i < 5; ++i) {
            pagebuf[col++] = g[i];
        }
        pagebuf[col++] = 0x00;
        ++s;
    }
}

static void display_apply_brightness(uint8_t percent)
{
    if (!s_inited) {
        return;
    }
    if (percent > 100) {
        percent = 100;
    }
    uint8_t contrast = (uint8_t)((percent * 255) / 100);
    if (contrast < 16) {
        contrast = 16;
    }
    ssd1306_write_cmd(0x81);
    ssd1306_write_cmd(contrast);
}

static const char *wifi_state_label(device_wifi_state_t state)
{
    switch (state) {
    case DEVICE_WIFI_CONNECTED: return "WiFi OK";
    case DEVICE_WIFI_CONNECTING: return "WiFi...";
    case DEVICE_WIFI_ERROR: return "WiFi ERR";
    default: return "Setup WiFi";
    }
}

static const char *ble_state_label(device_ble_state_t state)
{
    switch (state) {
    case DEVICE_BLE_CONNECTED: return "BLE Host";
    case DEVICE_BLE_PAIRING: return "BLE Pair";
    default: return "BLE Off";
    }
}

static int count_backup_files(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return 0;
    }
    file_manager_list(arr);
    int n = cJSON_GetArraySize(arr);
    cJSON_Delete(arr);
    return n;
}

static void display_render_home(void)
{
#if HAVE_OLED
    if (!s_inited) {
        return;
    }

    device_status_t st;
    device_status_get(&st);
    device_settings_t ds;
    settings_load(&ds);

    neo_usb_host_status_t usb = {0};
    usb_host_neo_get_host_status(&usb);
    const bool keyboard_live = usb.keyboard_active;

    char line[22];

    snprintf(line, sizeof(line), "%.20s", ds.device_name[0] ? ds.device_name : "Neo2 Buddy");
    render_text_to_pagebuf(line, s_pagebuf);
    ssd1306_update_page(0, s_pagebuf);

    if (HAVE_BATTERY) {
        snprintf(line, sizeof(line), "Bat %u%%%s  %s",
                 st.battery_percent,
                 st.charging ? "+" : " ",
                 wifi_state_label(st.wifi_state));
    } else {
        snprintf(line, sizeof(line), "Bat --   %s", wifi_state_label(st.wifi_state));
    }
    render_text_to_pagebuf(line, s_pagebuf);
    ssd1306_update_page(2, s_pagebuf);

    if (keyboard_live) {
        snprintf(line, sizeof(line), "KB Live  SD:%s", st.sd_card_mounted ? "Y" : "N");
    } else {
        int files = count_backup_files();
        snprintf(line, sizeof(line), "%s  SD:%s %d",
                 ble_state_label(st.ble_state),
                 st.sd_card_mounted ? "Y" : "N",
                 files);
    }
    render_text_to_pagebuf(line, s_pagebuf);
    ssd1306_update_page(4, s_pagebuf);

    if (keyboard_live) {
        char tail[20];
        unsigned long seq = 0;
        if (neo_live_tail(tail, sizeof(tail), &seq) != ESP_OK || tail[0] == '\0') {
            snprintf(line, sizeof(line), ">_");
        } else {
            snprintf(line, sizeof(line), ">%s", tail);
        }
    } else if (st.wifi_state == DEVICE_WIFI_CONNECTED && st.ip_address[0]) {
        snprintf(line, sizeof(line), "%s", st.ip_address);
    } else if (st.wifi_state == DEVICE_WIFI_UNCONFIGURED) {
        snprintf(line, sizeof(line), "AP: Neo2-Setup");
    } else if (st.wifi_ssid[0]) {
        snprintf(line, sizeof(line), "%.20s", st.wifi_ssid);
    } else {
        snprintf(line, sizeof(line), "No network");
    }
    render_text_to_pagebuf(line, s_pagebuf);
    ssd1306_update_page(6, s_pagebuf);
#endif
}

static void display_refresh_task(void *arg)
{
    (void)arg;
    for (;;) {
        neo_usb_host_status_t usb = {0};
        usb_host_neo_get_host_status(&usb);
        /* Refresh faster while Neo is typing so the OLED keeps up. */
        TickType_t wait = usb.keyboard_active ? pdMS_TO_TICKS(250) : pdMS_TO_TICKS(2000);
        ulTaskNotifyTake(pdTRUE, wait);
        if (s_inited && s_mode == DISPLAY_MODE_HOME) {
            display_render_home();
        }
    }
}

static esp_err_t ssd1306_try_open(void)
{
    if (s_oled_absent) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_inited) {
        return ESP_OK;
    }
    esp_err_t err = ssd1306_i2c_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed");
        return err;
    }
    if (ssd1306_probe() != ESP_OK) {
        (void)i2c_driver_delete(I2C_NUM_0);
        s_oled_absent = true;
        return ESP_ERR_NOT_FOUND;
    }
    ssd1306_init_display();
    s_inited = true;
    return ESP_OK;
}

esp_err_t display_init(void)
{
#if HAVE_OLED
    esp_err_t err = ssd1306_try_open();
    if (err != ESP_OK) {
        return err;
    }
    s_mode = DISPLAY_MODE_HOME;

    device_settings_t ds;
    if (settings_load(&ds) == ESP_OK) {
        display_apply_brightness(ds.display_brightness);
    }

    ensure_refresh_task();
    display_request_home();
    ESP_LOGI(TAG, "OLED home screen active");
#endif
    return ESP_OK;
}

void display_set_brightness(uint8_t percent)
{
    display_apply_brightness(percent);
}

void display_request_home(void)
{
    s_mode = DISPLAY_MODE_HOME;
    if (s_refresh_task != NULL) {
        xTaskNotifyGive(s_refresh_task);
    }
}

void display_show_home(void)
{
    display_request_home();
}

esp_err_t display_show_onboarding(const char *ssid, const char *password, const char *url)
{
#if HAVE_OLED
    if (ssd1306_try_open() != ESP_OK) {
        return ESP_FAIL;
    }
    ensure_refresh_task();
    s_mode = DISPLAY_MODE_ONBOARDING;

    /* We'll place three short lines on separate pages to avoid vertical
     * splitting: page 1, 3 and 5 (roughly top/middle/bottom). If the
     * provided strings are NULL we show placeholders. */
    const char *s1 = ssid ? ssid : "";
    const char *s2 = password ? password : "";
    const char *s3 = url ? url : "http://192.168.4.1/";

    render_text_to_pagebuf(s1, s_pagebuf);
    ssd1306_update_page(1, s_pagebuf);

    render_text_to_pagebuf(s2, s_pagebuf);
    ssd1306_update_page(3, s_pagebuf);

    render_text_to_pagebuf(s3, s_pagebuf);
    ssd1306_update_page(5, s_pagebuf);

    ESP_LOGI(TAG, "OLED onboarding shown: ssid='%s' pwd='%s' url='%s'", s1, s2, s3);
    log_buffer_appendf("display: onboarding shown ssid=%s pwd=%s url=%s", s1, s2, s3);
    return ESP_OK;
#else
    ESP_LOGI(TAG, "Onboarding (no-OLED): ssid=%s pwd=%s url=%s", ssid?ssid:"", password?password:"", url?url:"http://192.168.4.1/");
    return ESP_OK;
#endif
}
