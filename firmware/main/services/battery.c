/**
 * @file battery.c
 * @brief Battery voltage and percentage helpers.
 *
 * The module converts the ADC reading (in millivolts after the ADC scaler)
 * into an estimated battery voltage in millivolts and then maps that to a
 * human-friendly percentage using a small lookup/linear interpolation table.
 *
 * This is a simple resting-voltage approximation and not a full state-of-
 * charge estimator; it is suitable for basic UI reporting.
 */

#include "battery.h"
#include "board_config.h"

/* The ADC sees the divided voltage; `BOARD_BATTERY_DIVIDER_RATIO` accounts for
 * the resistor divider used on the board so callers can provide ADC measured
 * millivolts and receive the actual cell millivolts. */
uint16_t battery_voltage_from_adc_mv(uint16_t adc_millivolts)
{
    return (uint16_t)(adc_millivolts * BOARD_BATTERY_DIVIDER_RATIO);
}

/* Map a battery millivolt value to an approximate percentage using a small
 * monotonic curve table and linear interpolation between entries. */
uint8_t battery_percent_from_mv(uint16_t battery_millivolts)
{
    /* A compact, monotonic approximation of a resting single-cell Li-ion curve. */
    static const struct {
        uint16_t millivolts;
        uint8_t percent;
    } curve[] = {
        {3300, 0}, {3500, 5}, {3600, 10}, {3700, 25},
        {3800, 50}, {3900, 75}, {4000, 90}, {4200, 100},
    };

    if (battery_millivolts <= curve[0].millivolts) {
        return curve[0].percent;
    }

    for (size_t index = 1; index < sizeof(curve) / sizeof(curve[0]); index++) {
        if (battery_millivolts <= curve[index].millivolts) {
            uint16_t lower_mv = curve[index - 1].millivolts;
            uint16_t range_mv = curve[index].millivolts - lower_mv;
            uint8_t lower_percent = curve[index - 1].percent;
            uint8_t range_percent = curve[index].percent - lower_percent;
            return lower_percent + (uint8_t)((battery_millivolts - lower_mv) * range_percent / range_mv);
        }
    }

    return 100;
}