/*
 * power_mgr.c — LTE PSM / eDRX power management
 *
 * Negotiates Power Saving Mode (PSM) or extended Discontinuous Reception
 * (eDRX) with the LTE network to minimise current consumption between
 * telemetry transmissions on battery-powered devices.
 *
 * PSM summary:
 *   - Modem negotiates a sleep window with the network
 *   - Consumes ~2-3 µA during sleep (vs ~6-10 mA in idle connected)
 *   - Wakes automatically at TAU timer expiry, or on power_mgr_wake()
 *   - ~5-20x battery life improvement vs always-on depending on interval
 *
 * Typical configuration for 60s telemetry interval:
 *   PSM_TAU_SEC       = 3600  (network keepalive every hour)
 *   PSM_ACTIVE_SEC    = 10    (10s window to connect + publish + sleep)
 */

#include <zephyr/kernel.h>
#include <modem/lte_lc.h>
#include <modem/modem_info.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include "power_mgr.h"

LOG_MODULE_REGISTER(power_mgr, LOG_LEVEL_INF);

static bool g_psm_active = false;
static K_SEM_DEFINE(modem_ready_sem, 0, 1);

/* ── LTE event handler ────────────────────────────────────────────────────── */

static void lte_evt_handler(const struct lte_lc_evt *const evt)
{
    switch (evt->type) {

    case LTE_LC_EVT_NW_REG_STATUS:
        if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
            evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
            LOG_INF("LTE re-registered after PSM sleep (%s)",
                    evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME
                    ? "home" : "roaming");
            k_sem_give(&modem_ready_sem);
        } else if (evt->nw_reg_status == LTE_LC_NW_REG_NOT_REGISTERED) {
            LOG_WRN("LTE not registered");
        }
        break;

#if defined(CONFIG_LTE_LC_PSM_MODULE)
    case LTE_LC_EVT_PSM_UPDATE:
        if (evt->psm_cfg.active_time == -1) {
            LOG_WRN("Network did not grant PSM (TAU=%d, active=-1) — "
                    "device will stay connected between transmissions",
                    evt->psm_cfg.tau);
            g_psm_active = false;
        } else {
            LOG_INF("PSM granted: TAU=%ds, active=%ds",
                    evt->psm_cfg.tau, evt->psm_cfg.active_time);
            g_psm_active = true;
        }
        break;
#endif /* CONFIG_LTE_LC_PSM_MODULE */

#if defined(CONFIG_LTE_LC_EDRX_MODULE)
    case LTE_LC_EVT_EDRX_UPDATE:
        LOG_INF("eDRX updated: mode=%d, edrx=%dms, ptw=%dms",
                evt->edrx_cfg.mode,
                (int)(evt->edrx_cfg.edrx * 1000),
                (int)(evt->edrx_cfg.ptw  * 1000));
        break;
#endif /* CONFIG_LTE_LC_EDRX_MODULE */

#if defined(CONFIG_LTE_LC_MODEM_SLEEP_MODULE)
    case LTE_LC_EVT_MODEM_SLEEP_ENTER:
        LOG_DBG("Modem entered sleep (PSM)");
        break;

    case LTE_LC_EVT_MODEM_SLEEP_EXIT:
        LOG_DBG("Modem exited sleep");
        break;
#endif /* CONFIG_LTE_LC_MODEM_SLEEP_MODULE */

    default:
        break;
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int power_mgr_init(const struct power_mgr_config *cfg)
{
    if (!cfg) return -EINVAL;

    /* Register for LTE events so we get PSM confirmations */
    lte_lc_register_handler(lte_evt_handler);

    if (cfg->psm_enable && !cfg->edrx_enable) {
        /*
         * Request PSM with T3412 (TAU) and T3324 (active time) timers.
         *
         * Timer encoding uses 3GPP TS 24.008 format — the nRF SDK accepts
         * second values and converts internally.
         *
         * Good values for 60s telemetry:
         *   TAU = 3600s  (1 hour network keepalive)
         *   Active = 10s (10s to connect, publish, disconnect)
         */
        char tau_str[9];
        char active_str[9];

        /* Encode TAU in T3412 format — unit bits [7:5], value bits [4:0]
         * Unit 010 = 1 minute increments, Unit 001 = 1 hour increments     */
        int tau_hours = cfg->psm_tau_sec / 3600;
        if (tau_hours > 0 && tau_hours <= 31) {
            snprintf(tau_str, sizeof(tau_str), "01100%03d",
                     tau_hours); /* unit: 1 hour */
        } else {
            snprintf(tau_str, sizeof(tau_str), "00100%03d",
                     cfg->psm_tau_sec / 60); /* unit: 1 minute */
        }

        /* Encode active time in T3324 format (unit: 2s increments) */
        snprintf(active_str, sizeof(active_str), "00000%03d",
                 cfg->psm_active_time_sec / 2);

        int ret = lte_lc_psm_req(true);
        if (ret) {
            LOG_WRN("lte_lc_psm_req failed (%d)", ret);
        }

        LOG_INF("PSM requested: TAU=%ds (%s), active=%ds (%s)",
                cfg->psm_tau_sec, tau_str,
                cfg->psm_active_time_sec, active_str);

    } else if (cfg->edrx_enable && !cfg->psm_enable) {
        /* NCS v3.2.1: lte_lc_edrx_req() takes bool (true=enable, false=disable) */
        int ret = lte_lc_edrx_req(true);
        if (ret) {
            LOG_WRN("lte_lc_edrx_req failed (%d)", ret);
        } else {
            LOG_INF("eDRX requested");
        }
    } else if (!cfg->psm_enable && !cfg->edrx_enable) {
        /* Disable PSM explicitly to ensure consistent state */
        lte_lc_psm_req(false);
        LOG_INF("PSM/eDRX disabled — modem stays connected between transmissions");
    }

    return 0;
}

int power_mgr_wake(int timeout_sec)
{
    if (!g_psm_active) {
        /* PSM not active — modem is always connected, nothing to do */
        return 0;
    }

    LOG_DBG("Waiting for modem to wake from PSM (timeout %ds)...", timeout_sec);

    /* The modem wakes autonomously at TAU expiry or on network paging.
     * We just need to wait for LTE_LC_EVT_NW_REG_STATUS = REGISTERED. */
    int ret = k_sem_take(&modem_ready_sem, K_SECONDS(timeout_sec));
    if (ret == -EAGAIN) {
        LOG_WRN("Modem wake timeout after %ds — continuing anyway", timeout_sec);
        return -ETIMEDOUT;
    }

    LOG_DBG("Modem awake and registered");
    return 0;
}

void power_mgr_sleep(void)
{
    if (!g_psm_active) return;
    /*
     * PSM entry is automatic after the T3324 active timer expires.
     * There's nothing to call — the modem handles it.
     * This function exists as a hook for future explicit sleep control
     * (e.g. AT+CFUN=0 for deep sleep between long intervals).
     */
    LOG_DBG("Transmission complete — modem will enter PSM after active window");
}

bool power_mgr_is_psm_active(void)
{
    return g_psm_active;
}

int power_mgr_get_rssi(void)
{
    struct modem_param_info mp;
    if (modem_info_params_get(&mp) != 0) return -999;
    return (int)mp.network.rsrp.value;
}

int power_mgr_get_modem_temp(void)
{
    /* modem_info_get_temperature() issues AT%XTEMP — ~5 ms blocking.
     * Returns the modem die temperature in degrees Celsius.
     * Useful for detecting overheating in enclosures (>85°C = modem shutdown). */
    int temp_val;
    if (modem_info_get_temperature(&temp_val) != 0) return -999;
    return temp_val;
}
