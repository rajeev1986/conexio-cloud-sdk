#ifndef POWER_MGR_H
#define POWER_MGR_H

/**
 * @file power_mgr.h
 * @brief LTE PSM / eDRX power management
 *
 * Negotiates PSM or eDRX with the LTE network to minimise power consumption
 * between telemetry transmissions. Typical battery life improvement: 5-20x.
 *
 * Call power_mgr_init() once after LTE is registered.
 * Call power_mgr_wake() before MQTT activity.
 * Call power_mgr_sleep() when done transmitting — modem enters PSM.
 */

#include <stdbool.h>

/** PSM/eDRX configuration. */
struct power_mgr_config {
    bool  psm_enable;           /**< Enable PSM negotiation */
    int   psm_tau_sec;          /**< TAU timer (Tracking Area Update) */
    int   psm_active_time_sec;  /**< Active window duration */
    bool  edrx_enable;          /**< Enable eDRX (alternative to PSM) */
};

/**
 * @brief Initialise power management and negotiate PSM/eDRX with network.
 * Call after LTE is registered.
 * @param cfg  Power management configuration.
 * @return 0 on success.
 */
int power_mgr_init(const struct power_mgr_config *cfg);

/**
 * @brief Wake the modem for network activity.
 * Waits until the modem has re-registered if coming out of PSM sleep.
 * @param timeout_sec  Maximum seconds to wait for re-registration.
 * @return 0 on success, -ETIMEDOUT if modem doesn't wake in time.
 */
int power_mgr_wake(int timeout_sec);

/**
 * @brief Signal that network activity is complete — modem may enter PSM.
 * The modem will sleep until the next power_mgr_wake() call or TAU expiry.
 */
void power_mgr_sleep(void);

/**
 * @brief Returns true if PSM is active (modem may be sleeping).
 */
bool power_mgr_is_psm_active(void);

/**
 * @brief Returns last reported RSSI in dBm. -999 if unavailable.
 */
int power_mgr_get_rssi(void);

/**
 * @brief Returns the modem's internal temperature in degrees Celsius.
 *
 * Uses modem_info_get_temperature() which issues AT%XTEMP.
 * Returns -999 if unavailable (modem not initialised, or AT command failed).
 *
 * @note Only meaningful when the modem is in normal functional mode.
 */
int power_mgr_get_modem_temp(void);

#endif /* POWER_MGR_H */
