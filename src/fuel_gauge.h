/*
 * Copyright (c) 2026 Conexio Technologies, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * fuel_gauge.h — nPM13xx fuel gauge internal SDK header
 *
 * Provides battery voltage (mV), state-of-charge (%), and drain rate
 * (%/hr) to the SDK diagnostics payload via the nRF Fuel Gauge library.
 *
 * Activated by CONFIG_NRF_FUEL_GAUGE=y.
 * Called internally by conexio_cloud.c and power_mgr.c — not public API.
 *
 * ── Correct sleep architecture (NAN-045) ─────────────────────────────────
 *
 * The nRF Fuel Gauge is a Coulomb-counting algorithm. Nordic recommends:
 *
 *   Active (LTE connected / transmitting):
 *     Call nrf_fuel_gauge_process() at 1 Hz — accurate current measurement.
 *
 *   PSM sleep (modem in µA sleep):
 *     Do NOT call nrf_fuel_gauge_process() at all. Instead call
 *     nrf_fuel_gauge_idle_set(v, T, i_avg) once on sleep entry with the
 *     known average system current in sleep (typically 15–20 µA for Stratus Pro).
 *     The library integrates this fixed current internally until woken.
 *     This eliminates sensor_sample_fetch() wake-ups during PSM sleep.
 *
 *   On PSM exit:
 *     Resume nrf_fuel_gauge_process() calls — the library picks up from
 *     where idle_set() left off with no discontinuity in SOC.
 *
 * power_mgr.c calls conexio_fuel_gauge_on_sleep_enter/exit() from the
 * LTE_LC_EVT_MODEM_SLEEP_ENTER/EXIT handler so the transition is synchronous
 * with the modem event.
 */

#ifndef CONEXIO_FUEL_GAUGE_H__
#define CONEXIO_FUEL_GAUGE_H__

#include <zephyr/device.h>
#include <stdbool.h>

/**
 * @brief Initialise the nRF Fuel Gauge library and spawn the sampling thread.
 *
 * Acquires the pmic_charger device handle, performs an initial read,
 * calls nrf_fuel_gauge_init() with the LP963450 battery model, and starts
 * a background thread that calls nrf_fuel_gauge_process() at 1 Hz.
 *
 * Called automatically by battery_metrics_init() in conexio_cloud.c when
 * CONFIG_NRF_FUEL_GAUGE=y. No application code needed.
 *
 * @param charger  nPM13xx charger device handle (DT_NODELABEL(pmic_charger)).
 * @return 0 on success, negative errno on failure.
 */
int conexio_fuel_gauge_init(const struct device *charger);

/**
 * @brief Notify fuel gauge that the modem is entering PSM sleep.
 *
 * Called from power_mgr.c on LTE_LC_EVT_MODEM_SLEEP_ENTER.
 *
 * Reads current V and T from the nPM13xx, calls nrf_fuel_gauge_idle_set()
 * with CONFIG_CONEXIO_FUEL_GAUGE_PSM_SLEEP_CURRENT_UA, then suspends the
 * 1 Hz sampling thread. The library internally integrates the known sleep
 * current — no sampling occurs during PSM sleep, saving CPU wake-ups.
 */
void conexio_fuel_gauge_on_sleep_enter(void);

/**
 * @brief Notify fuel gauge that the modem is exiting PSM sleep.
 *
 * Called from power_mgr.c on LTE_LC_EVT_MODEM_SLEEP_EXIT.
 *
 * Resumes the 1 Hz sampling thread. nrf_fuel_gauge_process() calls pick
 * up where idle_set() left off — no discontinuity in SOC.
 */
void conexio_fuel_gauge_on_sleep_exit(void);

/**
 * @brief No-op stub — kept for API compatibility with conexio_cloud_publish().
 *
 * The sampling thread handles all updates continuously. This function does
 * nothing but return success when the gauge is ready.
 */
int conexio_fuel_gauge_update(const struct device *charger, bool vbus_connected);

/**
 * @brief Returns the latest cached battery voltage in millivolts.
 * @return Voltage in mV, or NAN if not yet sampled.
 */
double conexio_fuel_gauge_read_mv(void);

/**
 * @brief Returns the latest cached state-of-charge.
 * @return SOC in percent (0.0–100.0), or -1.0 if not yet sampled.
 */
float conexio_fuel_gauge_read_soc(void);

/**
 * @brief Returns true if the battery is currently charging (VBUS present).
 */
bool conexio_fuel_gauge_is_charging(void);

/**
 * @brief Returns true if fuel gauge init succeeded and the gauge is ready.
 */
bool conexio_fuel_gauge_is_ready(void);

#endif /* CONEXIO_FUEL_GAUGE_H__ */
