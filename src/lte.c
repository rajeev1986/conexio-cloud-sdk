/*
 * lte.c — LTE connection helper + passive session metrics
 *
 * Two responsibilities:
 *
 *  1. Bring up LTE:  conexio_lte_connect() starts the modem asynchronously
 *     and blocks on a semaphore until registration completes or times out.
 *
 *  2. Collect LTE session metrics: a persistent lte_lc event handler runs
 *     for the lifetime of the firmware and records the following without
 *     any extra AT commands or radio activity:
 *
 *     _lte_connect_ms   — boot-to-registered time in milliseconds
 *     _conn_loss        — number of drops + re-registrations since boot
 *     _reset_loop       — 1 if modem detected a reset loop this session
 *     _lte_mode         — 7=LTE-M, 9=NB-IoT (last reported mode)
 *     _cell_id          — E-UTRAN cell ID (last CELL_UPDATE)
 *     _tac              — Tracking Area Code (last CELL_UPDATE)
 *     _psm_tau_sec      — actual granted PSM TAU in seconds
 *     _psm_active_sec   — actual granted PSM active window in seconds
 *     _edrx_interval_ms — actual granted eDRX interval in milliseconds
 *     _edrx_ptw_ms      — actual granted eDRX paging time window in ms
 *
 * These are inspired by Memfault's ncs_lte_* metrics but fed directly
 * into Conexio telemetry instead of the Memfault pipeline.
 */

#include <zephyr/kernel.h>
#include <modem/lte_lc.h>
#include <zephyr/logging/log.h>
#include "lte.h"

LOG_MODULE_REGISTER(conexio_lte, LOG_LEVEL_INF);

/* ── Registration semaphore ──────────────────────────────────────────────
 *
 * Binary semaphore: 0 = waiting, 1 = registered.
 * conexio_lte_connect() takes it; the event handler gives it.
 */
static K_SEM_DEFINE(lte_ready_sem, 0, 1);

/* ── Session metrics ─────────────────────────────────────────────────────
 *
 * Updated in-place by lte_evt_handler() as events arrive.
 * conexio_lte_get_session_metrics() returns a read-only pointer to this.
 *
 * Initialised to "unknown" sentinel values so the cloud can distinguish
 * "not yet received" from "received value of 0".
 */
static struct conexio_lte_session_metrics g_metrics = {
    .connect_time_ms       = 0,
    .connection_loss_count = 0,
    .reset_loop_detected   = 0,
    .lte_mode              = 0,
    .cell_id               = 0xFFFFFFFF,  /* LTE_LC_CELL_EUTRAN_ID_INVALID */
    .tac                   = 0xFFFFFFFF,  /* LTE_LC_CELL_TAC_INVALID       */
    .psm_tau_sec           = -1,
    .psm_active_time_sec   = -1,
    .edrx_interval_ms      = 0,
    .edrx_ptw_ms           = 0,
};

/* Timestamp (ms) recorded when lte_lc_connect_async() is called.
 * Used to compute connect_time_ms when REGISTERED arrives. */
static int64_t g_connect_start_ms = 0;

/* Whether the modem was previously registered — used to detect drops. */
static bool g_was_registered = false;

/* ── LTE event handler ───────────────────────────────────────────────────
 *
 * Registered once and runs for the entire device lifetime.
 * Handles all LTE LC events relevant for observability metrics.
 */
static void lte_evt_handler(const struct lte_lc_evt *const evt)
{
    switch (evt->type) {

    /* ── Registration status ──────────────────────────────────────────── */
    case LTE_LC_EVT_NW_REG_STATUS:
        if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
            evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {

            if (!g_was_registered) {
                /* First registration this session — record boot-to-registered
                 * time, or count a re-registration after a drop. */
                if (g_metrics.connect_time_ms == 0 && g_connect_start_ms > 0) {
                    /* First-ever registration: calculate time to connect */
                    g_metrics.connect_time_ms =
                        (int32_t)(k_uptime_get() - g_connect_start_ms);
                    LOG_INF("LTE registered (%s) in %d ms",
                            evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME
                            ? "home" : "roaming",
                            g_metrics.connect_time_ms);
                } else {
                    /* Re-registration after a drop */
                    LOG_INF("LTE re-registered (%s)",
                            evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME
                            ? "home" : "roaming");
                }
                g_was_registered = true;
            }

            /* Release the semaphore so conexio_lte_connect() unblocks */
            k_sem_give(&lte_ready_sem);

        } else if (evt->nw_reg_status == LTE_LC_NW_REG_NOT_REGISTERED    ||
                   evt->nw_reg_status == LTE_LC_NW_REG_SEARCHING          ||
                   evt->nw_reg_status == LTE_LC_NW_REG_REGISTRATION_DENIED||
                   evt->nw_reg_status == LTE_LC_NW_REG_UNKNOWN            ||
                   evt->nw_reg_status == LTE_LC_NW_REG_UICC_FAIL) {

            if (g_was_registered) {
                /* Device was registered and just dropped — count a loss */
                g_metrics.connection_loss_count++;
                LOG_WRN("LTE connection lost (total losses: %u)",
                        g_metrics.connection_loss_count);
                g_was_registered = false;
            }
        }
        break;

    /* ── LTE mode (LTE-M=7 vs NB-IoT=9) ─────────────────────────────── */
    case LTE_LC_EVT_LTE_MODE_UPDATE:
        g_metrics.lte_mode = (uint8_t)evt->lte_mode;
        LOG_DBG("LTE mode: %d (%s)", g_metrics.lte_mode,
                g_metrics.lte_mode == 7 ? "LTE-M" :
                g_metrics.lte_mode == 9 ? "NB-IoT" : "other");
        break;

    /* ── Cell ID and TAC — updated when the device changes cells ─────── */
    case LTE_LC_EVT_CELL_UPDATE:
        g_metrics.cell_id = evt->cell.id;
        g_metrics.tac     = evt->cell.tac;
        LOG_DBG("Cell update: cell_id=0x%08X tac=0x%04X",
                g_metrics.cell_id, g_metrics.tac);
        break;

    /* ── PSM timers — actual values granted by the network ───────────── */
#if defined(CONFIG_LTE_LC_PSM_MODULE)
    case LTE_LC_EVT_PSM_UPDATE:
        g_metrics.psm_tau_sec         = evt->psm_cfg.tau;
        g_metrics.psm_active_time_sec = evt->psm_cfg.active_time;
        if (evt->psm_cfg.active_time == -1) {
            LOG_INF("PSM: network did not grant PSM (TAU=%ds)",
                    evt->psm_cfg.tau);
        } else {
            LOG_INF("PSM granted: TAU=%ds, active=%ds",
                    g_metrics.psm_tau_sec, g_metrics.psm_active_time_sec);
        }
        break;
#endif

    /* ── eDRX parameters — actual values granted by the network ──────── */
#if defined(CONFIG_LTE_LC_EDRX_MODULE)
    case LTE_LC_EVT_EDRX_UPDATE:
        /* edrx and ptw are float seconds in NCS v3.2.1 — convert to ms */
        g_metrics.edrx_interval_ms =
            (uint32_t)(evt->edrx_cfg.edrx * 1000.0f);
        g_metrics.edrx_ptw_ms =
            (uint32_t)(evt->edrx_cfg.ptw  * 1000.0f);
        LOG_INF("eDRX granted: interval=%.2fs ptw=%.2fs",
                (double)evt->edrx_cfg.edrx,
                (double)evt->edrx_cfg.ptw);
        break;
#endif

    /* ── Modem domain events ─────────────────────────────────────────── */
    case LTE_LC_EVT_MODEM_EVENT:
        if (evt->modem_evt.type == LTE_LC_MODEM_EVT_RESET_LOOP) {
            g_metrics.reset_loop_detected = 1;
            LOG_WRN("Modem reset loop detected — attach restricted for 30 min");
        }
        break;

    default:
        break;
    }
}

/* ── Public API ──────────────────────────────────────────────────────────*/

int conexio_lte_connect(int timeout_sec)
{
    LOG_INF("Connecting to LTE...");

    /* Record the start time for connect_time_ms calculation */
    g_connect_start_ms = k_uptime_get();

    /* Register the persistent event handler (safe to call multiple times —
     * lte_lc_register_handler is idempotent for the same function pointer) */
    lte_lc_register_handler(lte_evt_handler);

    /* Start the modem asynchronously.
     * NCS v3.2.1: lte_lc_init_and_connect_async() was removed; use
     * lte_lc_connect_async() which implicitly initialises on first call. */
    int ret = lte_lc_connect_async(lte_evt_handler);
    if (ret) {
        LOG_ERR("lte_lc_connect_async failed (%d)", ret);
        return ret;
    }

    /* Block until the event handler signals registration or timeout */
    ret = k_sem_take(&lte_ready_sem, K_SECONDS(timeout_sec));
    if (ret) {
        LOG_ERR("LTE registration timed out after %ds", timeout_sec);
        return -ETIMEDOUT;
    }

    return 0;
}

const struct conexio_lte_session_metrics *conexio_lte_get_session_metrics(void)
{
    return &g_metrics;
}
