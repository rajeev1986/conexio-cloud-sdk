/*
 * retry.c — Exponential backoff retry + hardware watchdog
 *
 * Exponential backoff prevents "thundering herd" — if many devices lose
 * connectivity simultaneously (e.g. after a network outage), they don't
 * all hammer the broker at the same moment when the network recovers.
 * Jitter (random ±base) spreads reconnection attempts over time.
 *
 * The hardware watchdog is the last line of defence: if the main loop
 * ever stops kicking the watchdog (due to a deadlock, modem hang, or
 * infinite blocking call), the device reboots automatically.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/device.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>   /* rand() */
#include <string.h>
#include "retry.h"

LOG_MODULE_REGISTER(retry, LOG_LEVEL_INF);

static struct retry_config g_cfg = {
    .base_sec       = 5,
    .max_sec        = 300,
    .max_attempts   = 10,
    .wdt_timeout_sec = 600,
};

static int  g_failure_count  = 0;
static bool g_backing_off    = false;

/* ── Hardware watchdog ────────────────────────────────────────────────────── */

static const struct device *wdt_dev;
static int wdt_channel_id = -1;

static int watchdog_init(int timeout_sec)
{
    wdt_dev = DEVICE_DT_GET(DT_NODELABEL(wdt0));

    if (!device_is_ready(wdt_dev)) {
        LOG_WRN("Watchdog device not ready — WDT disabled");
        wdt_dev = NULL;
        return 0; /* Non-fatal — continue without WDT */
    }

    struct wdt_timeout_cfg wdt_cfg = {
        .flags   = WDT_FLAG_RESET_SOC,
        .window  = {
            .min = 0U,
            .max = (uint32_t)timeout_sec * 1000U, /* ms */
        },
    };

    wdt_channel_id = wdt_install_timeout(wdt_dev, &wdt_cfg);
    if (wdt_channel_id < 0) {
        LOG_WRN("wdt_install_timeout failed (%d) — WDT disabled", wdt_channel_id);
        wdt_dev = NULL;
        return 0;
    }

    int ret = wdt_setup(wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG);
    if (ret) {
        LOG_WRN("wdt_setup failed (%d) — WDT disabled", ret);
        wdt_dev = NULL;
        return 0;
    }

    LOG_INF("Watchdog armed: %ds timeout", timeout_sec);
    return 0;
}

/* ── Backoff calculation ──────────────────────────────────────────────────── */

static int compute_backoff_sec(int attempt)
{
    /* base * 2^attempt, capped at max */
    int delay = g_cfg.base_sec;
    for (int i = 0; i < attempt && delay < g_cfg.max_sec; i++) {
        delay *= 2;
        if (delay > g_cfg.max_sec) delay = g_cfg.max_sec;
    }

    /* Add jitter: random [0, base_sec) to spread reconnections */
    int jitter = (g_cfg.base_sec > 0) ? (rand() % g_cfg.base_sec) : 0;
    delay += jitter;

    return delay;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int retry_init(const struct retry_config *cfg)
{
    if (cfg) {
        g_cfg.base_sec        = cfg->base_sec;
        g_cfg.max_sec         = cfg->max_sec;
        g_cfg.max_attempts    = cfg->max_attempts;
        g_cfg.wdt_timeout_sec = cfg->wdt_timeout_sec;
    }

    /* Seed jitter with uptime for different values each boot */
    srand((unsigned int)k_uptime_get_32());

    return watchdog_init(g_cfg.wdt_timeout_sec);
}

void retry_on_success(void)
{
    if (g_failure_count > 0) {
        LOG_INF("Connection restored after %d failure(s)", g_failure_count);
    }
    g_failure_count = 0;
    g_backing_off   = false;
}

int retry_on_failure(void)
{
    g_failure_count++;

    LOG_WRN("Connection failure #%d (max %d before reboot)",
            g_failure_count, g_cfg.max_attempts);

    if (g_failure_count >= g_cfg.max_attempts) {
        LOG_ERR("Max failures reached (%d) — rebooting to recover",
                g_failure_count);

        /*
         * Log why we're rebooting — useful for diagnosing field issues.
         * In production, write a reboot reason to NVS before rebooting
         * so the next boot can report it to the dashboard.
         */
        k_sleep(K_SECONDS(1)); /* flush logs */
        sys_reboot(SYS_REBOOT_COLD);

        return 0; /* unreachable */
    }

    int wait_sec = compute_backoff_sec(g_failure_count - 1);

    LOG_INF("Backoff: waiting %ds before retry (attempt %d/%d)",
            wait_sec, g_failure_count, g_cfg.max_attempts);

    g_backing_off = true;

    /*
     * Sleep in short increments so we keep kicking the watchdog.
     * A 300s backoff sleep must not block the watchdog kick.
     */
    int remaining = wait_sec;
    while (remaining > 0) {
        int chunk = MIN(remaining, (g_cfg.wdt_timeout_sec / 2));
        k_sleep(K_SECONDS(chunk));
        retry_kick_watchdog();
        remaining -= chunk;
    }

    g_backing_off = false;
    return wait_sec;
}

void retry_kick_watchdog(void)
{
    if (wdt_dev && wdt_channel_id >= 0) {
        wdt_feed(wdt_dev, wdt_channel_id);
    }
}

int retry_failure_count(void)
{
    return g_failure_count;
}

bool retry_is_backing_off(void)
{
    return g_backing_off;
}
