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
 * │    conexio_cloud_register_interval(min, max) — Golioth-style          │
 * └─────────────────────────────────────────────────────────────────────────┘
 */

/*
 * ONE include for everything — the SDK umbrella header automatically
 * includes fota.h, offline_buffer.h, power_mgr.h, retry.h based on Kconfig.
 */
#include <conexio_cloud/conexio_cloud.h>

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

/* ── Application state ────────────────────────────────────────────────── */
static int  g_alert_threshold = 80;
static bool g_debug_mode      = false;

/* ── Telemetry interval limits ────────────────────────────────────────── */
#define TELEMETRY_INTERVAL_MIN_S   10      /* Fastest: 10 s — debug / development  */
#define TELEMETRY_INTERVAL_MAX_S   7200    /* Slowest: 7200 s (2 h) — battery mode */

/* ── Alert threshold limits ───────────────────────────────────────────── */
#define ALERT_THRESHOLD_MIN        0       /* 0 = disabled                          */
#define ALERT_THRESHOLD_MAX        200     /* Covers full sensor range              */

/* Semaphore given when MQTT connects — gates the immediate boot publish. */
static K_SEM_DEFINE(cloud_connected_sem, 0, 1);

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
/* Return NAN to skip a reading for a given cycle (sensor unavailable). */
static double read_temperature(void *arg)
{
    ARG_UNUSED(arg);
    /* Simulated: random value in [22.0, 35.0] °C with 0.1 resolution */
    return 22.0 + (double)(sys_rand32_get() % 131) * 0.1;
}

static double read_humidity(void *arg)
{
    ARG_UNUSED(arg);
    /* Simulated: random value in [50.0, 80.0] % with 0.1 resolution */
    return 50.0 + (double)(sys_rand32_get() % 301) * 0.1;
}

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
    /* No range check needed — SDK validated against ALERT_THRESHOLD_MIN/MAX */
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
        /* Unblock the boot publish in main() */
        k_sem_give(&cloud_connected_sem);
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
     * SDK built-in: telemetryIntervalSec (CONFIG_AUTO_INTERVAL_SETTING)
     *
     * Use _with_range variants to declare valid limits as named constants.
     * The SDK validates incoming values before calling your handler —
     * no range check needed in the callback.                           */
    conexio_cloud_register_setting_int_with_range("alertThreshold",
                                                  ALERT_THRESHOLD_MIN,
                                                  ALERT_THRESHOLD_MAX,
                                                  on_alert_threshold, NULL);
    conexio_cloud_register_setting_bool("debugMode", on_debug_mode, NULL);

    /* ── Register telemetry interval with limits ──────────────────────
     * Golioth-style: declare the valid range once as named constants.
     * The SDK validates any SET_INTERVAL command against these limits —
     * no range check needed in application code.
     *
     * The optional callback is called ONLY when the new value is valid
     * and has been applied. Use it for app-level reactions (e.g. update
     * a display, persist to NVS, adjust a sensor duty cycle).
     * Pass NULL as the callback if no reaction is needed.              */
    conexio_cloud_register_interval(TELEMETRY_INTERVAL_MIN_S,
                                    TELEMETRY_INTERVAL_MAX_S);

    /* ── Single SDK init — handles everything ─────────────────────────
     * LTE connect → NTP sync → config fetch → cert provision →
     * transport init → PSM init → FOTA init → thread spawn           */
    int ret = conexio_cloud_init(cloud_event_handler);
    if (ret) {
        LOG_ERR("conexio_cloud_init failed (%d)", ret);
        return -1;
    }

    /* ── Immediate boot publish ───────────────────────────────────────
     * Wait for the SDK background thread to establish the MQTT connection,
     * then push all telemetry immediately — boot-once metrics
     * (_reboot_reason, _modem_fw, _lte_connect_ms, etc.) are included.
     * After this the SDK background thread publishes every INTERVAL_SEC. */
    LOG_INF("Waiting for MQTT connection before boot publish...");
    int sem_ret = k_sem_take(&cloud_connected_sem, K_SECONDS(60));
    if (sem_ret == 0) {
        LOG_INF("Boot publish — sending telemetry immediately after reset");
        int pub_ret = conexio_cloud_publish();
        if (pub_ret) {
            LOG_WRN("Boot publish failed (%d) — will retry at next interval", pub_ret);
        }
    } else {
        LOG_WRN("MQTT connect timeout — skipping boot publish");
    }

    /* ── Main loop — just sleep ───────────────────────────────────────
     * The SDK background thread reads sensors, publishes every
     * INTERVAL_SEC, manages PSM, buffers offline data, and handles
     * reconnects. conexio_cloud_get_interval_sec() reflects any runtime
     * changes from SET_INTERVAL or the telemetryIntervalSec OTA setting. */
    while (1) {
        k_sleep(K_SECONDS(conexio_cloud_get_interval_sec()));
    }

    return 0;
}
