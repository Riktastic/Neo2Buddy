/**
 * @file battery.h
 * @brief ADC millivolts to battery voltage and charge-percent helpers.
 *
 * Pure math — no hardware access. board_config.h supplies the divider ratio so
 * different PCB revisions only change compile-time constants. Percent is a
 * linear segment estimate for UI display, not coulomb counting.
 */

#pragma once

#include <stdint.h>

/** Convert raw ADC millivolts (before divider) to battery terminal millivolts. */
uint16_t battery_voltage_from_adc_mv(uint16_t adc_millivolts);

/**
 * Estimate Li-ion charge percent from battery millivolts.
 * Pass a filtered voltage; raw ADC noise will jitter the displayed percent.
 */
uint8_t battery_percent_from_mv(uint16_t battery_millivolts);

/** Nominal pack capacity in mAh (UI/runtime hints; default 2000 mAh). */
#define BATTERY_NOMINAL_CAPACITY_MAH 2000
