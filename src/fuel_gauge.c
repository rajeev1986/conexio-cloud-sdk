/*
 * fuel_gauge.c — nPM13xx / nRF Fuel Gauge SDK integration
 *
 * Moves battery fuel gauge management from the application layer into the
 * SDK so devices with CONFIG_NRF_FUEL_GAUGE=y automatically get accurate
 * battery voltage, state-of-charge, and drain rate in their diagnostics
 * payload without any application boilerplate.
 *
 * Battery model: LP963450 @ 25 °C (Conexio Stratus Pro default cell).
 * The model data is embedded from LP963450_25C.inc — same file previously
 * carried in the sample application.
 *
 * Integration with conexio_cloud.c:
 *   conexio_fuel_gauge_init()   — called inside battery_metrics_init()
 *                                  (under CONFIG_CONEXIO_CLOUD_BATTERY_METRICS)
 *   conexio_fuel_gauge_update() — called at the top of conexio_cloud_publish()
 *                                  to refresh VBUS state before battery_read_soc()
 *   conexio_fuel_gauge_read_mv()— called by the SDK's auto-registered
 *                                  _batt_mv sensor callback when
 *                                  CONFIG_CONEXIO_CLOUD_BATTERY_METRICS is off
 *
 * Devices WITHOUT the nPM13xx PMIC (CONFIG_NRF_FUEL_GAUGE=n):
 *   This file is not compiled. The SDK falls back to CONFIG_CONEXIO_CLOUD_AUTO_BATTERY
 *   (modem AT%%XVBAT) or the application registers its own _batt_mv sensor callback.
 */

#include <stdlib.h>
#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/npm13xx_charger.h>
#include <zephyr/logging/log.h>

#include <nrf_fuel_gauge.h>

#include "fuel_gauge.h"

LOG_MODULE_REGISTER(fuel_gauge, LOG_LEVEL_DBG);

/* ── Battery model ────────────────────────────────────────────────────────
 * LP963450 lithium polymer cell used in the Conexio Stratus Pro.
 * The .inc file contains the nRF Fuel Gauge model parameters struct.
 * It is #include'd directly into the battery_model initialiser below.
 */
static const struct battery_model battery_model = {
#include "LP963450_25C.inc"
};

/* ── nPM13xx charger status bitmasks ──────────────────────────────────── */
#define NPM13XX_CHG_STATUS_COMPLETE_MASK BIT(1)
#define NPM13XX_CHG_STATUS_TRICKLE_MASK  BIT(2)
#define NPM13XX_CHG_STATUS_CC_MASK       BIT(3)
#define NPM13XX_CHG_STATUS_CV_MASK       BIT(4)

/* ── Module state ─────────────────────────────────────────────────────── */
static bool    g_ready    = false;
static int64_t g_ref_time = 0;

/* ── Internal helpers ─────────────────────────────────────────────────── */

static int read_sensors(const struct device *charger,
                        float *voltage, float *current,
                        float *temp, int32_t *chg_status)
{
    struct sensor_value value;
    int ret;

    ret = sensor_sample_fetch(charger);
    if (ret < 0) {
        LOG_ERR("sensor_sample_fetch failed (%d)", ret);
        return ret;
    }

    ret = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &value);
    if (ret < 0) { LOG_ERR("GAUGE_VOLTAGE get failed (%d)", ret); return ret; }
    *voltage = (float)value.val1 + ((float)value.val2 / 1000000.0f);

    ret = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_TEMP, &value);
    if (ret < 0) {
        /* NTC not connected — use 25 °C as a safe default */
        LOG_DBG("GAUGE_TEMP not available (%d) — using 25 °C default", ret);
        value.val1 = 25;
        value.val2 = 0;
    }
    *temp = (float)value.val1 + ((float)value.val2 / 1000000.0f);

    ret = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_AVG_CURRENT, &value);
    if (ret < 0) { LOG_ERR("GAUGE_AVG_CURRENT get failed (%d)", ret); return ret; }
    /* Zephyr: negative = discharging. nRF Fuel Gauge expects opposite sign. */
    *current = -((float)value.val1 + ((float)value.val2 / 1000000.0f));

    ret = sensor_channel_get(charger, SENSOR_CHAN_NPM13XX_CHARGER_STATUS, &value);
    if (ret < 0) { LOG_ERR("CHARGER_STATUS get failed (%d)", ret); return ret; }
    *chg_status = value.val1;

    return 0;
}

static int charge_status_inform(int32_t chg_status)
{
    union nrf_fuel_gauge_ext_state_info_data state_info;

    if (chg_status & NPM13XX_CHG_STATUS_COMPLETE_MASK) {
        LOG_DBG("Charge complete");
        state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_COMPLETE;
    } else if (chg_status & NPM13XX_CHG_STATUS_TRICKLE_MASK) {
        LOG_DBG("Trickle charging");
        state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_TRICKLE;
    } else if (chg_status & NPM13XX_CHG_STATUS_CC_MASK) {
        LOG_DBG("Constant current charging");
        state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_CC;
    } else if (chg_status & NPM13XX_CHG_STATUS_CV_MASK) {
        LOG_DBG("Constant voltage charging");
        state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_CV;
    } else {
        LOG_DBG("Charger idle");
        state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_IDLE;
    }

    return nrf_fuel_gauge_ext_state_update(
        NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_STATE_CHANGE, &state_info);
}

/* ── Public API ───────────────────────────────────────────────────────── */

int conexio_fuel_gauge_init(const struct device *charger)
{
    if (!charger || !device_is_ready(charger)) {
        LOG_ERR("pmic_charger device not ready — fuel gauge unavailable");
        return -ENODEV;
    }

    struct nrf_fuel_gauge_init_parameters parameters = {
        .model      = &battery_model,
        .opt_params = NULL,
        .state      = NULL,
    };

    float max_charge_current;
    float term_charge_current;
    int32_t chg_status;
    struct sensor_value value;
    int ret;

    LOG_INF("nRF Fuel Gauge version: %s", nrf_fuel_gauge_version);

    ret = read_sensors(charger,
                       &parameters.v0, &parameters.i0,
                       &parameters.t0, &chg_status);
    if (ret < 0) {
        LOG_ERR("Initial sensor read failed (%d)", ret);
        return ret;
    }

    /* Fetch nominal and termination charge current for TTE/TTF calculation */
    ret = sensor_channel_get(charger,
                             SENSOR_CHAN_GAUGE_DESIRED_CHARGING_CURRENT, &value);
    if (ret < 0) {
        LOG_WRN("DESIRED_CHARGING_CURRENT unavailable (%d) — TTE/TTF disabled",
                ret);
        max_charge_current  = 0.0f;
        term_charge_current = 0.0f;
    } else {
        max_charge_current  = (float)value.val1 + (float)value.val2 / 1000000.0f;
        term_charge_current = max_charge_current / 10.0f;
    }

    ret = nrf_fuel_gauge_init(&parameters, NULL);
    if (ret < 0) {
        LOG_ERR("nrf_fuel_gauge_init failed (%d)", ret);
        return ret;
    }

    if (max_charge_current > 0.0f) {
        nrf_fuel_gauge_ext_state_update(
            NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_CURRENT_LIMIT,
            &(union nrf_fuel_gauge_ext_state_info_data){
                .charge_current_limit = max_charge_current });

        nrf_fuel_gauge_ext_state_update(
            NRF_FUEL_GAUGE_EXT_STATE_INFO_TERM_CURRENT,
            &(union nrf_fuel_gauge_ext_state_info_data){
                .charge_term_current = term_charge_current });
    }

    ret = charge_status_inform(chg_status);
    if (ret < 0) {
        LOG_WRN("charge_status_inform failed (%d) — continuing", ret);
    }

    g_ref_time = k_uptime_get();
    g_ready    = true;

    LOG_INF("nPM13xx fuel gauge initialised (V0=%.3f V, I0=%.3f A, T0=%.1f °C)",
            (double)parameters.v0,
            (double)(-parameters.i0), /* display as positive discharge current */
            (double)parameters.t0);

    return 0;
}

int conexio_fuel_gauge_update(const struct device *charger, bool vbus_connected)
{
    if (!g_ready || !charger) {
        return -ENODEV;
    }

    static int32_t chg_status_prev = -1;
    float voltage, current, temp;
    int32_t chg_status;
    int ret;

    ret = read_sensors(charger, &voltage, &current, &temp, &chg_status);
    if (ret < 0) {
        LOG_WRN("fuel_gauge_update: sensor read failed (%d)", ret);
        return ret;
    }

    /* Inform of VBUS state change */
    nrf_fuel_gauge_ext_state_update(
        vbus_connected ? NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_CONNECTED
                       : NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_DISCONNECTED,
        NULL);

    /* Inform of charge status change (only when it actually changes) */
    if (chg_status != chg_status_prev) {
        chg_status_prev = chg_status;
        ret = charge_status_inform(chg_status);
        if (ret < 0) {
            LOG_WRN("charge_status_inform failed (%d)", ret);
        }
    }

    /* Advance the Coulomb-counting algorithm */
    float delta = (float)k_uptime_delta(&g_ref_time) / 1000.0f;

    float soc = nrf_fuel_gauge_process(voltage, current, temp, delta, NULL);
    float tte = nrf_fuel_gauge_tte_get();
    float ttf = nrf_fuel_gauge_ttf_get();

    LOG_DBG("V: %.3fV  I: %.3fA  T: %.1f°C  SoC: %.1f%%  TTE: %.0fs  TTF: %.0fs",
            (double)voltage,
            (double)(-current), /* display as positive discharge */
            (double)temp,
            (double)soc,
            (double)tte,
            (double)ttf);

    return 0;
}

double conexio_fuel_gauge_read_mv(void)
{
    if (!g_ready) {
        return (double)NAN;
    }

    /* Use the device handle resolved at init time via conexio_cloud.c */
    extern const struct device *g_pmic_charger_dev;
    const struct device *charger = g_pmic_charger_dev;

    if (!charger) {
        return (double)NAN;
    }

    struct sensor_value voltage;
    int ret = sensor_sample_fetch(charger);
    if (ret < 0) {
        LOG_WRN("sensor_sample_fetch failed (%d)", ret);
        return (double)NAN;
    }

    ret = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &voltage);
    if (ret < 0) {
        LOG_WRN("GAUGE_VOLTAGE get failed (%d)", ret);
        return (double)NAN;
    }

    double mv = ((double)voltage.val1 * 1000.0) +
                ((double)voltage.val2 / 1000.0);

    LOG_DBG("battery: %.3f V (%d mV)", mv / 1000.0, (int)mv);
    return mv;
}

bool conexio_fuel_gauge_is_ready(void)
{
    return g_ready;
}
