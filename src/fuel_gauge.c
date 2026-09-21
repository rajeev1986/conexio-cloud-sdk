/*
 * fuel_gauge.c — nPM13xx / nRF Fuel Gauge SDK integration
 *
 * ── Architecture (NAN-045 compliant) ──────────────────────────────────────
 *
 * Active state (LTE connected):
 *   fg_thread calls nrf_fuel_gauge_process() at 1 Hz
 *   (CONFIG_CONEXIO_FUEL_GAUGE_ACTIVE_RATE_MS, default 1000 ms).
 *   Accurate Coulomb counting at the rate Nordic recommends.
 *
 * PSM sleep (modem in µA range):
 *   conexio_fuel_gauge_on_sleep_enter() is called from power_mgr.c on
 *   LTE_LC_EVT_MODEM_SLEEP_ENTER:
 *     1. Read V and T from the nPM13xx one last time.
 *     2. Call nrf_fuel_gauge_idle_set(v, T, i_avg) where i_avg is the
 *        known system quiescent current in sleep (CONFIG_CONEXIO_FUEL_GAUGE_PSM_SLEEP_CURRENT_UA,
 *        default 20 µA for the Stratus Pro: nRF9151 ~2.5 µA + nPM13xx ~8 µA + misc ~10 µA).
 *     3. Suspend fg_thread — zero sensor_sample_fetch() wakes during sleep.
 *   The library integrates i_avg × elapsed_time internally.
 *
 *   conexio_fuel_gauge_on_sleep_exit() is called on LTE_LC_EVT_MODEM_SLEEP_EXIT:
 *     1. Resume fg_thread — process() calls pick up seamlessly.
 *
 * The publish path (conexio_cloud.c battery_read_soc()) reads atomically
 * from the cache — no extra sensor fetch at publish time.
 *
 * ── Integration ───────────────────────────────────────────────────────────
 *   conexio_fuel_gauge_init()         — called by battery_metrics_init()
 *   conexio_fuel_gauge_on_sleep_enter/exit() — called by power_mgr.c
 *   conexio_fuel_gauge_update()       — no-op, kept for API compat
 *   conexio_fuel_gauge_read_mv/soc/is_charging() — read cache
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

LOG_MODULE_REGISTER(fuel_gauge, LOG_LEVEL_INF);

/* ── Battery model ────────────────────────────────────────────────────────
 * LP963450 lithium polymer cell (Conexio Stratus Pro).
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
static bool                g_ready       = false;
static int64_t             g_ref_time    = 0;
static const struct device *g_charger_dev = NULL;

/* PSM sleep flag — set by on_sleep_enter, cleared by on_sleep_exit.
 * Volatile so the compiler doesn't optimise out the thread's read. */
static volatile bool g_sleeping = false;

/* Semaphore used to resume fg_thread after PSM sleep exit.
 * The thread takes it (blocking) when g_sleeping is set;
 * on_sleep_exit() gives it to unblock the thread. */
static K_SEM_DEFINE(fg_resume_sem, 0, 1);

/* Cached values — written by fg_thread, read by publish path.
 * Spinlock ensures atomic read across all callers. */
static struct k_spinlock g_cache_lock;
static float  g_cached_soc_pct  = -1.0f;
static double g_cached_mv       = NAN;
static bool   g_cached_charging = false;

/* Sampling thread */
static K_THREAD_STACK_DEFINE(fg_stack, CONFIG_CONEXIO_FUEL_GAUGE_STACK_SIZE);
static struct k_thread fg_thread_data;

/* ── Internal helpers ─────────────────────────────────────────────────── */

static int read_sensors(const struct device *charger,
                        float *voltage, float *current,
                        float *temp, int32_t *chg_status)
{
    struct sensor_value value;
    int ret;

    ret = sensor_sample_fetch(charger);
    if (ret < 0) { LOG_ERR("sensor_sample_fetch failed (%d)", ret); return ret; }

    ret = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &value);
    if (ret < 0) { LOG_ERR("GAUGE_VOLTAGE get failed (%d)", ret); return ret; }
    *voltage = (float)value.val1 + (float)value.val2 / 1000000.0f;

    ret = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_TEMP, &value);
    if (ret < 0) {
        LOG_DBG("GAUGE_TEMP unavailable (%d) — using 25 °C default", ret);
        value.val1 = 25; value.val2 = 0;
    }
    *temp = (float)value.val1 + (float)value.val2 / 1000000.0f;

    ret = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_AVG_CURRENT, &value);
    if (ret < 0) { LOG_ERR("GAUGE_AVG_CURRENT get failed (%d)", ret); return ret; }
    /* Zephyr: negative = discharging. nRF FG expects opposite. */
    *current = -((float)value.val1 + (float)value.val2 / 1000000.0f);

    ret = sensor_channel_get(charger, SENSOR_CHAN_NPM13XX_CHARGER_STATUS, &value);
    if (ret < 0) { LOG_ERR("CHARGER_STATUS get failed (%d)", ret); return ret; }
    *chg_status = value.val1;

    return 0;
}

static int charge_status_inform(int32_t chg_status)
{
    union nrf_fuel_gauge_ext_state_info_data si;

    if      (chg_status & NPM13XX_CHG_STATUS_COMPLETE_MASK) {
        LOG_DBG("Charger: complete");
        si.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_COMPLETE;
    } else if (chg_status & NPM13XX_CHG_STATUS_TRICKLE_MASK) {
        LOG_DBG("Charger: trickle");
        si.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_TRICKLE;
    } else if (chg_status & NPM13XX_CHG_STATUS_CC_MASK) {
        LOG_DBG("Charger: CC");
        si.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_CC;
    } else if (chg_status & NPM13XX_CHG_STATUS_CV_MASK) {
        LOG_DBG("Charger: CV");
        si.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_CV;
    } else {
        LOG_DBG("Charger: idle");
        si.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_IDLE;
    }

    return nrf_fuel_gauge_ext_state_update(
        NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_STATE_CHANGE, &si);
}

/* ── 1 Hz sampling thread ─────────────────────────────────────────────────
 *
 * Runs continuously when the modem is active.
 * On PSM sleep enter: suspends itself by blocking on fg_resume_sem.
 * On PSM sleep exit:  unblocked by conexio_fuel_gauge_on_sleep_exit().
 */
static void fg_thread_fn(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

    static int32_t chg_status_prev = -1;

    while (1) {
        k_sleep(K_MSEC(CONFIG_CONEXIO_FUEL_GAUGE_ACTIVE_RATE_MS));

        /* ── Suspend during PSM sleep ─────────────────────────────────── */
        if (g_sleeping) {
            LOG_DBG("FG thread suspended (PSM sleep)");
            k_sem_take(&fg_resume_sem, K_FOREVER);
            LOG_DBG("FG thread resumed (PSM exit)");
            /* Reset delta timer — the idle_set period has already been
             * accounted for by the library; start fresh from now. */
            g_ref_time = k_uptime_get();
        }

        if (!g_ready || !g_charger_dev) {
            continue;
        }

        /* ── Sample nPM13xx ───────────────────────────────────────────── */
        float voltage, current, temp;
        int32_t chg_status;

        if (read_sensors(g_charger_dev,
                         &voltage, &current, &temp, &chg_status) < 0) {
            continue;
        }

        /* Charging: fuel gauge sign convention — positive = charging,
         * which is negative in Zephyr (already negated in read_sensors). */
        bool is_charging = (current < 0.0f);

        nrf_fuel_gauge_ext_state_update(
            is_charging ? NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_CONNECTED
                        : NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_DISCONNECTED,
            NULL);

        if (chg_status != chg_status_prev) {
            chg_status_prev = chg_status;
            charge_status_inform(chg_status);
        }

        /* ── Advance Coulomb counter ──────────────────────────────────── */
        float delta_s = (float)k_uptime_delta(&g_ref_time) / 1000.0f;
        float soc     = nrf_fuel_gauge_process(voltage, current, temp,
                                                delta_s, NULL);

        LOG_DBG("FG: V=%.3fV I=%.3fA T=%.1f°C SOC=%.1f%% "
                "TTE=%.0fs TTF=%.0fs",
                (double)voltage, (double)(-current), (double)temp, (double)soc,
                (double)nrf_fuel_gauge_tte_get(),
                (double)nrf_fuel_gauge_ttf_get());

        /* ── Update cache ─────────────────────────────────────────────── */
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
        LOG_ERR("pmic_charger not ready — fuel gauge unavailable");
        return -ENODEV;
    }

    struct nrf_fuel_gauge_init_parameters params = {
        .model      = &battery_model,
        .opt_params = NULL,
        .state      = NULL,
    };

    float max_charge_current = 0.0f;
    float term_charge_current = 0.0f;
    int32_t chg_status;
    struct sensor_value value;
    int ret;

    LOG_INF("nRF Fuel Gauge version: %s", nrf_fuel_gauge_version);

    ret = read_sensors(charger,
                       &params.v0, &params.i0, &params.t0, &chg_status);
    if (ret < 0) { LOG_ERR("Initial sensor read failed (%d)", ret); return ret; }

    ret = sensor_channel_get(charger,
                             SENSOR_CHAN_GAUGE_DESIRED_CHARGING_CURRENT, &value);
    if (ret < 0) {
        LOG_WRN("DESIRED_CHARGING_CURRENT unavailable — TTE/TTF disabled");
    } else {
        max_charge_current  = (float)value.val1 + (float)value.val2 / 1000000.0f;
        term_charge_current = max_charge_current / 10.0f;
    }

    ret = nrf_fuel_gauge_init(&params, NULL);
    if (ret < 0) { LOG_ERR("nrf_fuel_gauge_init failed (%d)", ret); return ret; }

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

    charge_status_inform(chg_status);

    g_charger_dev = charger;
    g_ref_time    = k_uptime_get();
    g_ready       = true;

    /* Seed cache with initial voltage */
    k_spinlock_key_t key = k_spin_lock(&g_cache_lock);
    g_cached_soc_pct = 0.0f;
    g_cached_mv      = (double)params.v0 * 1000.0;
    k_spin_unlock(&g_cache_lock, key);

    k_thread_create(&fg_thread_data, fg_stack,
                    K_THREAD_STACK_SIZEOF(fg_stack),
                    fg_thread_fn, NULL, NULL, NULL,
                    K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);
    k_thread_name_set(&fg_thread_data, "fuel_gauge");

    LOG_INF("nPM13xx fuel gauge ready — 1 Hz sampling, PSM sleep: idle_set @ %d µA",
            CONFIG_CONEXIO_FUEL_GAUGE_PSM_SLEEP_CURRENT_UA);

    return 0;
}

void conexio_fuel_gauge_on_sleep_enter(void)
{
    if (!g_ready || !g_charger_dev) {
        return;
    }

    /* Read current V and T — final sample before the modem sleeps.
     * These are passed to nrf_fuel_gauge_idle_set() so the library can
     * use them as the starting point for its internal idle integration. */
    float voltage = 0.0f, current = 0.0f, temp = 25.0f;
    int32_t chg_status = 0;
    read_sensors(g_charger_dev, &voltage, &current, &temp, &chg_status);

    /* Expected system quiescent current during PSM sleep.
     * Convert from µA (Kconfig) to A (fuel gauge library unit). */
    const float i_sleep_a =
        (float)CONFIG_CONEXIO_FUEL_GAUGE_PSM_SLEEP_CURRENT_UA / 1000000.0f;

    /* Hand off to the library — it integrates i_sleep × elapsed internally. */
    nrf_fuel_gauge_idle_set(voltage, temp, i_sleep_a);

    LOG_DBG("FG: idle_set V=%.3fV T=%.1f°C i_sleep=%.1f µA",
            (double)voltage, (double)temp,
            (double)CONFIG_CONEXIO_FUEL_GAUGE_PSM_SLEEP_CURRENT_UA);

    /* Signal the sampling thread to suspend */
    g_sleeping = true;
}

void conexio_fuel_gauge_on_sleep_exit(void)
{
    if (!g_ready) {
        return;
    }

    /* Clear the sleep flag and unblock the sampling thread.
     * The thread resets g_ref_time after waking so process() delta
     * starts from the moment of PSM exit, not from the last process() call. */
    g_sleeping = false;
    k_sem_give(&fg_resume_sem);

    LOG_DBG("FG: PSM exit — resuming 1 Hz sampling");
}

int conexio_fuel_gauge_update(const struct device *charger, bool vbus_connected)
{
    /* No-op: thread handles updates. Kept for conexio_cloud_publish() compat. */
    ARG_UNUSED(charger);
    ARG_UNUSED(vbus_connected);
    return g_ready ? 0 : -ENODEV;
}

double conexio_fuel_gauge_read_mv(void)
{
    if (!g_ready) return (double)NAN;
    k_spinlock_key_t key = k_spin_lock(&g_cache_lock);
    double mv = g_cached_mv;
    k_spin_unlock(&g_cache_lock, key);
    return mv;
}

float conexio_fuel_gauge_read_soc(void)
{
    if (!g_ready) return -1.0f;
    k_spinlock_key_t key = k_spin_lock(&g_cache_lock);
    float soc = g_cached_soc_pct;
    k_spin_unlock(&g_cache_lock, key);
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
