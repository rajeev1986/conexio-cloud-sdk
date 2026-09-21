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
 * ── Sampling strategy ─────────────────────────────────────────────────────
 *
 * Nordic recommends sampling the nPM13xx at different rates depending on
 * modem activity, because the nRF Fuel Gauge is a Coulomb-counting algorithm:
 * the `delta` parameter to nrf_fuel_gauge_process() must accurately reflect
 * elapsed time between calls for the SOC estimate to stay accurate.
 *
 *   Modem active / transmitting:  1 Hz (CONFIG_CONEXIO_FUEL_GAUGE_ACTIVE_RATE_MS)
 *   Modem in PSM sleep:           30 s (CONFIG_CONEXIO_FUEL_GAUGE_PSM_RATE_MS)
 *
 * A dedicated background thread (fg_thread) runs this loop continuously,
 * independent of the telemetry publish interval. The cached SOC and voltage
 * values are read atomically by conexio_cloud_publish() without an extra
 * sensor_sample_fetch().
 *
 * Thread stack size is controlled by CONFIG_CONEXIO_FUEL_GAUGE_STACK_SIZE.
 *
 * Integration with conexio_cloud.c:
 *   conexio_fuel_gauge_init()       — called inside battery_metrics_init()
 *                                     (under CONFIG_CONEXIO_CLOUD_BATTERY_METRICS)
 *                                     spawns the sampling thread
 *   conexio_fuel_gauge_update()     — called at the top of conexio_cloud_publish()
 *                                     returns the latest cached SOC/voltage
 *   conexio_fuel_gauge_read_mv()    — returns cached battery voltage in mV
 *
 * Devices WITHOUT nPM13xx (CONFIG_NRF_FUEL_GAUGE=n):
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
#include "power_mgr.h"

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

/* Cached values written by the sampling thread, read by publish path.
 * Protected by a spinlock so reads are always atomic across cores. */
static struct k_spinlock g_cache_lock;
static float  g_cached_soc_pct  = -1.0f;  /* -1 = not yet sampled */
static double g_cached_mv       = NAN;
static bool   g_cached_charging = false;

/* Saved device pointer so the thread can use it after init */
static const struct device *g_charger_dev;

/* Background sampling thread */
static K_THREAD_STACK_DEFINE(fg_stack,
                              CONFIG_CONEXIO_FUEL_GAUGE_STACK_SIZE);
static struct k_thread fg_thread_data;

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

/* ── Sampling thread ──────────────────────────────────────────────────────
 *
 * Runs continuously after fuel gauge init.
 * Samples at 1 Hz (active) or every CONFIG_CONEXIO_FUEL_GAUGE_PSM_RATE_MS
 * (PSM sleep) so the Coulomb counter stays accurate.
 *
 * Adaptive rate:
 *   - Modem in PSM sleep (g_psm_sleeping == true): long interval — no point
 *     sampling at 1 Hz when the modem draws only microamps in sleep; the
 *     current is so small that the SOC barely changes.
 *   - Modem active: sample at CONFIG_CONEXIO_FUEL_GAUGE_ACTIVE_RATE_MS (default 1000ms)
 *     per Nordic's recommendation for accurate Coulomb counting.
 */
static void fg_thread_fn(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

    static int32_t chg_status_prev = -1;

    while (1) {
        /* ── Choose sample interval based on modem state ─────────────── */
#if defined(CONFIG_CONEXIO_CLOUD_PSM)
        /* When PSM is active and the modem is in sleep (not actively
         * transmitting), use the longer interval to save power.
         * power_mgr_is_psm_active() returns true if the network granted PSM.
         * We use that as the proxy for "modem may be in sleep" — the modem
         * manages its own sleep/wake cycle via TAU and active timers.     */
        bool in_psm = power_mgr_is_psm_active();
        k_sleep(in_psm
                ? K_MSEC(CONFIG_CONEXIO_FUEL_GAUGE_PSM_RATE_MS)
                : K_MSEC(CONFIG_CONEXIO_FUEL_GAUGE_ACTIVE_RATE_MS));
#else
        k_sleep(K_MSEC(CONFIG_CONEXIO_FUEL_GAUGE_ACTIVE_RATE_MS));
#endif

        if (!g_ready || !g_charger_dev) {
            continue;
        }

        /* ── Sample the nPM13xx ───────────────────────────────────────── */
        float voltage, current, temp;
        int32_t chg_status;

        if (read_sensors(g_charger_dev,
                         &voltage, &current, &temp, &chg_status) < 0) {
            continue;
        }

        /* Detect charging from current sign (before negation) */
        bool is_charging = (current < 0.0f); /* Fuel gauge sign: positive=charging */

        /* Update VBUS / charge state when changed */
        nrf_fuel_gauge_ext_state_update(
            is_charging ? NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_CONNECTED
                        : NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_DISCONNECTED,
            NULL);

        if (chg_status != chg_status_prev) {
            chg_status_prev = chg_status;
            charge_status_inform(chg_status);
        }

        /* ── Advance the Coulomb-counting algorithm ───────────────────── */
        float delta = (float)k_uptime_delta(&g_ref_time) / 1000.0f;
        float soc   = nrf_fuel_gauge_process(voltage, current, temp,
                                              delta, NULL);

        LOG_DBG("FG sample: V=%.3fV I=%.3fA T=%.1f°C SoC=%.1f%% "
                "TTE=%.0fs TTF=%.0fs",
                (double)voltage,
                (double)(-current),
                (double)temp,
                (double)soc,
                (double)nrf_fuel_gauge_tte_get(),
                (double)nrf_fuel_gauge_ttf_get());

        /* ── Update cache atomically ──────────────────────────────────── */
        k_spinlock_key_t key = k_spin_lock(&g_cache_lock);
        g_cached_soc_pct  = soc;
        g_cached_mv       = (double)voltage * 1000.0;
        g_cached_charging = is_charging;
        k_spin_unlock(&g_cache_lock, key);
    }
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

    g_ref_time    = k_uptime_get();
    g_charger_dev = charger;
    g_ready       = true;

    /* Seed the cache with the initial reading */
    k_spinlock_key_t key = k_spin_lock(&g_cache_lock);
    g_cached_soc_pct = 0.0f;   /* will be updated on first thread cycle */
    g_cached_mv      = (double)parameters.v0 * 1000.0;
    k_spin_unlock(&g_cache_lock, key);

    /* Spawn the continuous sampling thread */
    k_thread_create(&fg_thread_data, fg_stack,
                    K_THREAD_STACK_SIZEOF(fg_stack),
                    fg_thread_fn, NULL, NULL, NULL,
                    K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);
    k_thread_name_set(&fg_thread_data, "fuel_gauge");

    LOG_INF("nPM13xx fuel gauge ready — sampling at %d ms (active) / %d ms (PSM)",
            CONFIG_CONEXIO_FUEL_GAUGE_ACTIVE_RATE_MS,
            CONFIG_CONEXIO_FUEL_GAUGE_PSM_RATE_MS);

    return 0;
}

int conexio_fuel_gauge_update(const struct device *charger, bool vbus_connected)
{
    /* No-op: the sampling thread handles continuous updates.
     * This function is kept for API compatibility — connexio_cloud_publish()
     * calls it, but the actual work is already done by fg_thread. */
    ARG_UNUSED(charger);
    ARG_UNUSED(vbus_connected);
    return g_ready ? 0 : -ENODEV;
}

double conexio_fuel_gauge_read_mv(void)
{
    if (!g_ready) {
        return (double)NAN;
    }

    k_spinlock_key_t key = k_spin_lock(&g_cache_lock);
    double mv = g_cached_mv;
    k_spin_unlock(&g_cache_lock, key);

    return mv;
}

float conexio_fuel_gauge_read_soc(void)
{
    if (!g_ready) {
        return -1.0f;
    }

    k_spinlock_key_t key = k_spin_lock(&g_cache_lock);
    float soc = g_cached_soc_pct;
    bool  chg = g_cached_charging;
    k_spin_unlock(&g_cache_lock, key);

    ARG_UNUSED(chg);
    return soc;
}

bool conexio_fuel_gauge_is_charging(void)
{
    k_spinlock_key_t key = k_spin_lock(&g_cache_lock);
    bool chg = g_cached_charging;
    k_spin_unlock(&g_cache_lock, key);
    return chg;
}

bool conexio_fuel_gauge_is_ready(void)
{
    return g_ready;
}
