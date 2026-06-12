#ifndef RETRY_H
#define RETRY_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @file retry.h
 * @brief Exponential backoff retry strategy + hardware watchdog
 *
 * Manages reconnection attempts with jittered exponential backoff
 * and a hardware watchdog that reboots the device if the main loop
 * stalls (catches modem hangs, deadlocks, infinite retry loops).
 *
 * Backoff formula:
 *   delay = min(base * 2^attempt, max) + random_jitter(0, base)
 *
 * Example with base=5, max=300:
 *   Attempt 0:  5 + jitter  ≈  5-10s
 *   Attempt 1: 10 + jitter  ≈ 10-15s
 *   Attempt 2: 20 + jitter  ≈ 20-25s
 *   Attempt 3: 40 + jitter  ≈ 40-45s
 *   Attempt 4: 80 + jitter  ≈ 80-85s
 *   Attempt 5+: 300 + jitter ≈ 300-305s (capped)
 */

/** Retry configuration. */
struct retry_config {
    int base_sec;       /**< Base retry interval */
    int max_sec;        /**< Maximum retry interval (cap) */
    int max_attempts;   /**< Reboot after this many consecutive failures */
    int wdt_timeout_sec;/**< Hardware watchdog timeout */
};

/**
 * @brief Initialise retry module and start hardware watchdog.
 * @param cfg  Retry configuration.
 * @return 0 on success.
 */
int retry_init(const struct retry_config *cfg);

/**
 * @brief Report a successful connection — resets failure counter.
 */
void retry_on_success(void);

/**
 * @brief Report a connection failure.
 * Returns the number of seconds to wait before next attempt.
 * Reboots the device if max_attempts is reached.
 * @return Seconds to wait (with jitter applied).
 */
int retry_on_failure(void);

/**
 * @brief Kick the hardware watchdog — call from main loop.
 * Must be called at least once per wdt_timeout_sec seconds.
 */
void retry_kick_watchdog(void);

/**
 * @brief Returns the current consecutive failure count.
 */
int retry_failure_count(void);

/**
 * @brief Returns true if we are currently in a backoff wait period.
 */
bool retry_is_backing_off(void);

#endif /* RETRY_H */
