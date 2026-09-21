/*
 * fuel_gauge.h — nPM13xx fuel gauge internal SDK header
 *
 * Provides battery voltage (mV), state-of-charge (%), and drain rate
 * (%/hr) to the SDK diagnostics payload via the nRF Fuel Gauge library.
 *
 * Activated by CONFIG_NRF_FUEL_GAUGE=y.
 * Called internally by conexio_cloud.c — not part of the public SDK API.
 */

#ifndef CONEXIO_FUEL_GAUGE_H__
#define CONEXIO_FUEL_GAUGE_H__

#include <zephyr/device.h>
#include <stdbool.h>

/**
 * @brief Initialise the nRF Fuel Gauge library.
 *
 * Acquires the pmic_charger device handle, performs an initial sensor
 * sample_fetch, and calls nrf_fuel_gauge_init() with the LP963450 battery
 * model. Must be called once before fuel_gauge_read_mv() or the SDK's
 * battery_read_soc() will produce valid readings.
 *
 * @param charger  nPM13xx charger device handle (DT_NODELABEL(pmic_charger)).
 *                 Pass NULL to let the function resolve it automatically.
 * @return 0 on success, negative errno on failure.
 */
int conexio_fuel_gauge_init(const struct device *charger);

/**
 * @brief Update VBUS connection state in the fuel gauge library.
 *
 * Informs nrf_fuel_gauge_ext_state_update() of VBUS connect/disconnect
 * events so it can adjust the Coulomb-counting algorithm for charging.
 * Called once per publish cycle in conexio_cloud_publish().
 *
 * @param charger        nPM13xx charger device handle.
 * @param vbus_connected true if VBUS is present (USB/charger plugged in).
 * @return 0 on success, negative errno on failure.
 */
int conexio_fuel_gauge_update(const struct device *charger, bool vbus_connected);

/**
 * @brief Read battery terminal voltage from the nPM13xx fuel gauge.
 *
 * Performs a sensor_sample_fetch + SENSOR_CHAN_GAUGE_VOLTAGE read.
 * Returns the value in millivolts. Returns NAN on any error.
 *
 * Only used when CONFIG_CONEXIO_CLOUD_BATTERY_METRICS is not set
 * (i.e. the SDK doesn't call battery_read_soc() which already caches
 * voltage). When BATTERY_METRICS is enabled the cached g_last_battery_mv
 * from battery_read_soc() is used instead.
 *
 * @return Battery voltage in mV, or NAN on error.
 */
double conexio_fuel_gauge_read_mv(void);

/**
 * @brief Returns true if fuel_gauge_init succeeded and the gauge is ready.
 */
bool conexio_fuel_gauge_is_ready(void);

/**
 * @brief Returns the latest cached state-of-charge from the sampling thread.
 * @return SOC in percent (0.0–100.0), or -1.0 if not yet sampled.
 */
float conexio_fuel_gauge_read_soc(void);

/**
 * @brief Returns true if the battery is currently charging (VBUS connected).
 */
bool conexio_fuel_gauge_is_charging(void);

#endif /* CONEXIO_FUEL_GAUGE_H__ */
