/**
 * @file battery_monitor.c
 * @brief Periodically sample the battery ADC and publish status.
 */

#include "battery_monitor.h"
#include "battery.h"
#include "board_config.h"
#include "device_status.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "battery_mon";

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_cali_ok;

static bool init_adc(void)
{
    adc_unit_t unit;
    adc_channel_t channel;
    if (adc_oneshot_io_to_channel(BOARD_BATTERY_ADC_GPIO, &unit, &channel) != ESP_OK) {
        ESP_LOGW(TAG, "Battery ADC: GPIO%d is not an ADC pin", (int)BOARD_BATTERY_ADC_GPIO);
        return false;
    }
    if (unit != ADC_UNIT_1) {
        ESP_LOGW(TAG, "Battery ADC: GPIO%d is not on ADC1", (int)BOARD_BATTERY_ADC_GPIO);
        return false;
    }

    adc_oneshot_unit_init_cfg_t init = {
        .unit_id = unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&init, &s_adc) != ESP_OK) {
        return false;
    }

    adc_oneshot_chan_cfg_t chan = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    if (adc_oneshot_config_channel(s_adc, channel, &chan) != ESP_OK) {
        return false;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_cali_ok = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) == ESP_OK;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_cali_ok = adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali) == ESP_OK;
#else
    s_cali_ok = false;
#endif
    return true;
}

static uint16_t read_battery_mv(void)
{
    adc_unit_t unit;
    adc_channel_t channel;
    if (adc_oneshot_io_to_channel(BOARD_BATTERY_ADC_GPIO, &unit, &channel) != ESP_OK) {
        return 0;
    }

    int raw = 0;
    if (adc_oneshot_read(s_adc, channel, &raw) != ESP_OK) {
        return 0;
    }

    int mv = 0;
    if (s_cali_ok && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
        return battery_voltage_from_adc_mv((uint16_t)mv);
    }

    /* Fallback: approximate 3.3 V full-scale at 12 dB attenuation. */
    return battery_voltage_from_adc_mv((uint16_t)((raw * 3300) / 4095));
}

static void battery_monitor_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint16_t mv = read_battery_mv();
        uint8_t pct = battery_percent_from_mv(mv);
        device_status_set_battery(mv, pct, false);
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

esp_err_t battery_monitor_init(void)
{
#if HAVE_BATTERY
    if (!init_adc()) {
        ESP_LOGW(TAG, "Battery ADC init failed");
        return ESP_FAIL;
    }
    if (xTaskCreate(battery_monitor_task, "battery_mon", 3072, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Battery monitor started");
    return ESP_OK;
#else
    return ESP_OK;
#endif
}
