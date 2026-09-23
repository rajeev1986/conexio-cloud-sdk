/*
 * Copyright (c) 2026 Conexio Technologies, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * device_settings/src/main.c — OTA Config (device settings) sample.
 *
 * Device Settings (OTA Config) allow the Conexio Console to push JSON
 * configuration to the device at runtime — no firmware reflash needed.
 * Settings are delivered via MQTT (QoS 1) to the device's config topic.
 *
 * Built-in SDK settings (handled automatically, no code needed):
 *   telemetryIntervalSec — changes the publish interval at runtime
 *
 * Application settings demonstrated in this sample:
 *   reportingMode (string)  — "normal" | "verbose" | "silent"
 *   alertThreshold (int)    — temperature alert trigger (°C)
 *
 * How to test from Conexio Console:
 *   1. Go to your device → OTA Config tab
 *   2. Add a setting:
 *        Key:   alertThreshold
 *        Type:  Integer
 *        Value: 30
 *   3. Click Push — the device receives it within one MQTT poll cycle
 *   4. Serial log shows: "Setting: alertThreshold → 30"
 *
 * Settings are not persisted in flash by the SDK — on reboot the device
 * reverts to its compiled defaults. The Console pushes the current settings
 * again on reconnect if "push on reconnect" is configured.
 */

#include <conexio_cloud/conexio_cloud.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <math.h>   /* NAN — returned by read_uptime in silent mode */
#include <string.h> /* strcmp, strncpy */

#if __has_include(<app_version.h>)
#  include <app_version.h>
#else
#  define APP_VERSION_STRING "1.0.0"
#endif

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

/* ── Application state — updated by settings handlers ──────────────────── */
static int  g_alert_threshold = 35;          /* °C — alert fires above this */
static char g_reporting_mode[16] = "normal"; /* "normal" | "verbose" | "silent" */

/* ── Settings handlers ────────────────────────────────────────────────────
 * Return CONEXIO_SETTING_OK to accept the value.
 * Return CONEXIO_SETTING_ERROR to reject it (Console marks setting as failed).
 *
 * For int settings use _with_range to have the SDK validate bounds before
 * calling your handler — no manual range check needed in the callback.
 */

static enum conexio_setting_status on_alert_threshold(int32_t value, void *arg)
{
    ARG_UNUSED(arg);
    g_alert_threshold = (int)value;
    LOG_INF("Setting: alertThreshold → %d °C", g_alert_threshold);
    return CONEXIO_SETTING_OK;
}

static enum conexio_setting_status on_reporting_mode(const char *value, void *arg)
{
    ARG_UNUSED(arg);
    if (strcmp(value, "normal")  != 0 &&
        strcmp(value, "verbose") != 0 &&
        strcmp(value, "silent")  != 0) {
        LOG_WRN("Setting: reportingMode — unknown value '%s', rejecting", value);
        return CONEXIO_SETTING_ERROR;
    }
    strncpy(g_reporting_mode, value, sizeof(g_reporting_mode) - 1);
    LOG_INF("Setting: reportingMode → %s", g_reporting_mode);
    return CONEXIO_SETTING_OK;
}

/* ── Sensor callback — shows settings in use ────────────────────────────── */
static double read_uptime(void *arg)
{
    ARG_UNUSED(arg);
    if (strcmp(g_reporting_mode, "silent") == 0) {
        return (double)NAN;   /* suppress metric in silent mode */
    }
    return (double)(k_uptime_get() / 1000);
}

/* ── Cloud event handler ────────────────────────────────────────────────── */
static void on_cloud_event(const struct conexio_cloud_event *evt)
{
    switch (evt->type) {
    case CONEXIO_CLOUD_EVT_CONNECTED:
        LOG_INF("Connected — %s | threshold=%d°C mode=%s",
                conexio_cloud_device_id(),
                g_alert_threshold, g_reporting_mode);
        break;
    case CONEXIO_CLOUD_EVT_DISCONNECTED:
        LOG_WRN("Disconnected");
        break;
    case CONEXIO_CLOUD_EVT_ERROR:
        LOG_ERR("Cloud error: %d", evt->data.error);
        break;
    default:
        break;
    }
}

/* ── main ─────────────────────────────────────────────────────────────── */
int main(void)
{
    LOG_INF("=== Conexio Device Settings Sample ===");
    LOG_INF("Defaults: threshold=%d°C mode=%s",
            g_alert_threshold, g_reporting_mode);

    /* Register sensor so we have something to publish */
    conexio_cloud_register_sensor("uptime_sec", read_uptime, NULL);

    /*
     * Register application settings.
     * telemetryIntervalSec is built-in (CONFIG_CONEXIO_CLOUD_AUTO_INTERVAL_SETTING=y)
     * and does not need to be registered here.
     */
    conexio_cloud_register_setting_int_with_range(
        "alertThreshold", 0, 100, on_alert_threshold, NULL);
    conexio_cloud_register_setting_string(
        "reportingMode", on_reporting_mode, NULL);

    int ret = conexio_cloud_init(on_cloud_event);
    if (ret) { LOG_ERR("init failed (%d)", ret); return -1; }

    LOG_INF("Ready — push settings from Conexio Console → OTA Config.");
    if (conexio_cloud_wait_connected(60000) == 0) {
        k_sleep(K_SECONDS(2));
        conexio_cloud_publish();
    }

    while (1) {
        k_sleep(K_SECONDS(5));
    }
    return 0;
}
