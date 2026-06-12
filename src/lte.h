#ifndef CONEXIO_LTE_H
#define CONEXIO_LTE_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @file lte.h
 * @brief LTE connection helper + session metrics
 *
 * Exposes conexio_lte_connect() for bringing up LTE, and
 * conexio_lte_get_session_metrics() for reading per-session
 * observability counters collected from LTE LC events.
 *
 * These metrics are collected passively inside the LTE event handler —
 * no extra AT commands, no extra overhead on the radio.
 */

/**
 * @brief LTE session metrics collected from lte_lc events.
 *
 * All counters are reset to zero when conexio_lte_connect() is called
 * (i.e. at each boot / each new LTE session).  They accumulate until
 * the device reboots or conexio_lte_connect() is called again.
 *
 * These map directly to Memfault's ncs_lte_* metrics but are stored
 * locally and published via the Conexio telemetry pipeline instead.
 */
struct conexio_lte_session_metrics {
    /** Time from lte_lc_connect_async() call to first registration, ms.
     *  Useful for detecting SIM/coverage issues on boot. */
    int32_t connect_time_ms;

    /** Number of times the modem dropped and re-registered since boot.
     *  Each drop+re-register increments this counter by 1. */
    uint16_t connection_loss_count;

    /** Whether the modem detected a reset loop (restricted attach for 30 min).
     *  1 = reset loop detected this session, 0 = normal. */
    uint8_t reset_loop_detected;

    /** Active LTE mode at last LTE_MODE_UPDATE event.
     *  7 = LTE-M, 9 = NB-IoT, 0 = unknown/none. */
    uint8_t lte_mode;

    /** E-UTRAN cell ID at last CELL_UPDATE event (decimal).
     *  0xFFFFFFFF = invalid/unknown. */
    uint32_t cell_id;

    /** Tracking Area Code (TAC) at last CELL_UPDATE event.
     *  0xFFFFFFFF = invalid/unknown. */
    uint32_t tac;

    /** Actual PSM TAU granted by the network (seconds), -1 = not granted. */
    int32_t psm_tau_sec;

    /** Actual PSM active time granted by the network (seconds), -1 = not granted. */
    int32_t psm_active_time_sec;

    /** Actual eDRX interval granted (milliseconds), 0 = not active. */
    uint32_t edrx_interval_ms;

    /** Actual eDRX paging time window (milliseconds), 0 = not active. */
    uint32_t edrx_ptw_ms;
};

/**
 * @brief Bring up LTE and wait for registration.
 *
 * Starts the modem asynchronously and blocks until the modem registers
 * on the home network or while roaming.  Also starts collecting LTE
 * session metrics via the internal event handler.
 *
 * @param timeout_sec Maximum seconds to wait for registration.
 * @return 0 on success, -ETIMEDOUT on timeout, negative errno on failure.
 */
int conexio_lte_connect(int timeout_sec);

/**
 * @brief Return a pointer to the current LTE session metrics.
 *
 * The returned pointer is valid for the lifetime of the firmware image.
 * Metrics are updated in-place by the LTE event handler as events arrive.
 * Call this just before building a telemetry payload to get current values.
 *
 * @return Pointer to the static session metrics struct. Never NULL.
 */
const struct conexio_lte_session_metrics *conexio_lte_get_session_metrics(void);

#endif /* CONEXIO_LTE_H */
