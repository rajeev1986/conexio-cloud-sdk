/*
 * main.c — Conexio Advanced Sample Application
 *
 * nRF Connect SDK v3.2.1 / nRF91xx
 *
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │  This is what an application built on conexio-cloud-sdk-v2 looks like. │
 * │                                                                         │
 * │  The application provides:                                              │
 * │    1. Sensor reading callbacks (registered — SDK calls them)            │
 * │    2. Actuator command handlers (FAN_ON, FAN_OFF)                       │
 * │    3. OTA Config settings handlers (alertThreshold, debugMode)          │
 * │    4. Optional cloud event handler (status LED etc.)                    │
 * │    5. main(): register → init → sleep loop                              │
 * │                                                                         │
 * │  The SDK provides everything else — no boilerplate needed:              │
 * │    REBOOT, SET_INTERVAL, FIRMWARE_UPDATE commands — built-in            │
 * │    telemetryIntervalSec setting — built-in                              │
 * │    Retry, WDT, PSM, offline buffer, FOTA — enabled via prj.conf        │
 * │    _rssi, _snr, _reboot_cnt, _battery_mv, _sdk_version — auto-metrics  │
 * └─────────────────────────────────────────────────────────────────────────┘
 */

/*
 * ONE include for everything — the SDK umbrella header automatically
 * includes fota.h, offline_buffer.h, power_mgr.h, retry.h based on Kconfig.
 */
#include <conexio_cloud/conexio_cloud.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

/* ── Application state ────────────────────────────────────────────────── */
static int  g_alert_threshold = 80;
static bool g_debug_mode      = false;

/* ── Sensor callbacks ─────────────────────────────────────────────────── */
/*
 * These are called by the SDK background thread before each publish.
 * Replace the stub returns with your actual Zephyr sensor driver calls:
 *
 *   static double read_temperature(void *arg) {
 *       struct sensor_value val;
 *       sensor_sample_fetch(temp_dev);
 *       sensor_channel_get(temp_dev, SENSOR_CHAN_AMBIENT_TEMP, &val);
 *       return sensor_value_to_double(&val);
 *   }
 *
 * Return NAN to skip a reading for a given cycle (sensor unavailable).
 */
static double read_temperature(void *arg) { ARG_UNUSED(arg); return 22.5; }
static double read_humidity(void *arg)    { ARG_UNUSED(arg); return 61.0; }

/* ── Command handlers — hardware-specific only ────────────────────────── */
/*
 * REBOOT, SET_INTERVAL, FIRMWARE_UPDATE are registered automatically by
 * the SDK.  Only add handlers here for YOUR hardware commands.
 */

static void on_fan_on(const char *payload_json, void *arg)
{
    ARG_UNUSED(payload_json); ARG_UNUSED(arg);
    /* Payload example: {"speed": 80} — the SDK passes the raw JSON string.
     * Parse with cJSON if you need the speed value. */
    LOG_INF("FAN_ON");
    /* TODO: gpio_pin_set(fan_dev, FAN_PIN, 1); */
}

static void on_fan_off(const char *payload_json, void *arg)
{
    ARG_UNUSED(payload_json); ARG_UNUSED(arg);
    LOG_INF("FAN_OFF");
    /* TODO: gpio_pin_set(fan_dev, FAN_PIN, 0); */
}

/* ── Settings handlers — app-specific keys only ───────────────────────── */
/*
 * telemetryIntervalSec is handled automatically by the SDK
 * (CONFIG_CONEXIO_CLOUD_AUTO_INTERVAL_SETTING=y).
 * Only register handlers for keys specific to your application.
 */

static enum conexio_setting_status on_alert_threshold(int32_t value, void *arg)
{
    ARG_UNUSED(arg);
    if (value < 0 || value > 200) return CONEXIO_SETTING_VALUE_OUT_OF_RANGE;
    g_alert_threshold = (int)value;
    LOG_INF("Setting: alertThreshold → %d", g_alert_threshold);
    return CONEXIO_SETTING_OK;
}

static enum conexio_setting_status on_debug_mode(bool value, void *arg)
{
    ARG_UNUSED(arg);
    g_debug_mode = value;
    LOG_INF("Setting: debugMode → %s", g_debug_mode ? "ON" : "OFF");
    return CONEXIO_SETTING_OK;
}

/* ── Cloud event handler — application reactions only ─────────────────── */
/*
 * The SDK handles all internal housekeeping before calling this:
 *   CONNECTED    → retry_on_success, buffer replay, fota_check_and_execute
 *   DISCONNECTED → retry_on_failure (with backoff)
 *   PUBLISHED    → power_mgr_sleep
 *   ERROR        → retry_on_failure
 *
 * Only add code here for app-level reactions (status LEDs, buzzer, etc.).
 * Pass NULL to conexio_cloud_init() if you have no app-level reactions.
 */
static void cloud_event_handler(const struct conexio_cloud_event *evt)
{
    switch (evt->type) {
    case CONEXIO_CLOUD_EVT_CONNECTED:
        LOG_INF("Connected — %s | SDK %s",
                conexio_cloud_device_id(), conexio_cloud_version());
        /* TODO: status LED green */
        break;
    case CONEXIO_CLOUD_EVT_DISCONNECTED:
        LOG_WRN("Disconnected");
        /* TODO: status LED red */
        break;
    case CONEXIO_CLOUD_EVT_PUBLISHED:
        LOG_DBG("Published");
        break;
    case CONEXIO_CLOUD_EVT_ERROR:
        LOG_ERR("Cloud error: %d", evt->data.error);
        break;
    default:
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════════════*/

int main(void)
{
    LOG_INF("=== Conexio Advanced Sample (SDK %s) ===",
            conexio_cloud_version());

    /* ── Register sensor callbacks ────────────────────────────────────
     * The SDK calls these before each publish — no send_metric in loop. */
    conexio_cloud_register_sensor("temperature", read_temperature, NULL);
    conexio_cloud_register_sensor("humidity",    read_humidity,    NULL);

    /* ── Register application commands ───────────────────────────────
     * SDK built-ins: REBOOT, SET_INTERVAL, FIRMWARE_UPDATE            */
    conexio_cloud_register_command("FAN_ON",  on_fan_on,  NULL);
    conexio_cloud_register_command("FAN_OFF", on_fan_off, NULL);

    /* ── Register application settings ───────────────────────────────
     * SDK built-in: telemetryIntervalSec (CONFIG_AUTO_INTERVAL_SETTING) */
    conexio_cloud_register_setting_int( "alertThreshold", on_alert_threshold, NULL);
    conexio_cloud_register_setting_bool("debugMode",      on_debug_mode,      NULL);

    /* ── Single SDK init — handles everything ─────────────────────────
     * LTE connect → NTP sync → config fetch → cert provision →
     * transport init → PSM init → FOTA init → thread spawn           */
    int ret = conexio_cloud_init(cloud_event_handler);
    if (ret) {
        LOG_ERR("conexio_cloud_init failed (%d)", ret);
        return -1;
    }

    /* ── Main loop — just sleep ───────────────────────────────────────
     * The SDK background thread reads sensors, publishes, manages PSM,
     * buffers offline data, kicks the watchdog, and handles reconnects.
     * conexio_cloud_get_interval_sec() reflects any runtime changes from
     * SET_INTERVAL or the telemetryIntervalSec OTA Config setting.     */
    while (1) {
        k_sleep(K_SECONDS(conexio_cloud_get_interval_sec()));
    }

    return 0;
}
