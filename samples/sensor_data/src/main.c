/*
 * Copyright (c) 2026 Conexio Technologies, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * sensor_data/src/main.c — Sensor data streaming sample.
 *
 * Demonstrates fetching sensor readings (temperature, humidity) and
 * streaming them to Conexio Cloud on a configurable interval.
 *
 * Metric behaviour:
 *
 *   CONFIG_CONEXIO_SAMPLE_SIMULATED_SENSORS=y (default)
 *     "temperature"      — random 20–40 °C  (simulated)
 *     "humidity"         — random 40–80 %   (simulated)
 *     "temperature_real" — SHT4x reading    (NAN if sensor absent)
 *     "humidity_real"    — SHT4x reading    (NAN if sensor absent)
 *
 *   CONFIG_CONEXIO_SAMPLE_SIMULATED_SENSORS=n
 *     "temperature"      — SHT4x reading (real sensor only)
 *     "humidity"         — SHT4x reading (real sensor only)
 *
 * The SDK handles:
 *   - LTE, MQTT, CBOR encoding, retry, PSM
 *   - SDK auto-metrics: _rssi, _snr, _reboot_cnt, _sdk_version, etc.
 */

#include <conexio_cloud/conexio_cloud.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/random/random.h>
#include <zephyr/logging/log.h>
#include <math.h>

#if __has_include(<app_version.h>)
#  include <app_version.h>
#else
#  define APP_VERSION_STRING "1.0.0"
#endif

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

/* Alert fires when temperature exceeds this value (°C).
 * Update via Conexio Console → OTA Config → alertThreshold setting. */
static int g_alert_threshold = 35;

/* ── Real SHT4x hardware device ─────────────────────────────────────────
 * Initialised in main(); NULL on boards without the DT node.
 * Both real-sensor callbacks guard against NULL / not-ready before use.
 */
#if DT_HAS_COMPAT_STATUS_OKAY(sensirion_sht4x)
static const struct device *s_sht4x;
#endif

/* ── Real SHT4x callbacks (defined first so simulated=n can forward to them)
 *
 * Each callback fetches a fresh sample independently and checks every return
 * value. Returns NAN when the sensor is absent, not ready, fetch fails, or
 * the channel read fails — the SDK silently skips NAN values so the sample
 * builds and runs on boards without the sensor physically attached.
 */
#if DT_HAS_COMPAT_STATUS_OKAY(sensirion_sht4x)

static double read_temperature_real(void *arg)
{
    ARG_UNUSED(arg);
    if (s_sht4x == NULL || !device_is_ready(s_sht4x)) {
        return (double)NAN;
    }
    struct sensor_value val;
    if (sensor_sample_fetch(s_sht4x) < 0) {
        LOG_WRN("SHT4x: sample fetch failed");
        return (double)NAN;
    }
    if (sensor_channel_get(s_sht4x, SENSOR_CHAN_AMBIENT_TEMP, &val) < 0) {
        LOG_WRN("SHT4x: temperature channel read failed");
        return (double)NAN;
    }
    return sensor_value_to_double(&val);
}

static double read_humidity_real(void *arg)
{
    ARG_UNUSED(arg);
    if (s_sht4x == NULL || !device_is_ready(s_sht4x)) {
        return (double)NAN;
    }
    /* Independent fetch per callback — no ordering dependency. */
    struct sensor_value val;
    if (sensor_sample_fetch(s_sht4x) < 0) {
        LOG_WRN("SHT4x: sample fetch failed (humidity)");
        return (double)NAN;
    }
    if (sensor_channel_get(s_sht4x, SENSOR_CHAN_HUMIDITY, &val) < 0) {
        LOG_WRN("SHT4x: humidity channel read failed");
        return (double)NAN;
    }
    return sensor_value_to_double(&val);
}

#else  /* board has no sensirion_sht4x DT node */

static double read_temperature_real(void *arg) { ARG_UNUSED(arg); return (double)NAN; }
static double read_humidity_real(void *arg)    { ARG_UNUSED(arg); return (double)NAN; }

#endif /* DT_HAS_COMPAT_STATUS_OKAY(sensirion_sht4x) */

/* ── Primary sensor callbacks ("temperature" / "humidity") ───────────────
 *
 * SIMULATED_SENSORS=y → random values (default, good for cloud integration tests)
 * SIMULATED_SENSORS=n → forwards to SHT4x real callbacks above so the standard
 *   metric names always carry valid data and dashboard widgets never need updating.
 */
#if defined(CONFIG_CONEXIO_SAMPLE_SIMULATED_SENSORS)
#warning "Simulated sensors active — set CONFIG_CONEXIO_SAMPLE_SIMULATED_SENSORS=n for production."

static double read_temperature(void *arg)
{
    ARG_UNUSED(arg);
    double temp = 20.0 + (double)(sys_rand32_get() % 200) * 0.1; /* 20–40 °C */
    if (g_alert_threshold > 0 && temp >= (double)g_alert_threshold) {
        conexio_cloud_publish_alert("temperature", temp, (double)g_alert_threshold);
        LOG_WRN("Alert: temperature %.1f °C exceeds threshold %d °C",
                temp, g_alert_threshold);
    }
    return temp;
}

static double read_humidity(void *arg)
{
    ARG_UNUSED(arg);
    return 40.0 + (double)(sys_rand32_get() % 400) * 0.1; /* 40–80 % */
}

#else  /* SIMULATED_SENSORS=n — forward to SHT4x */

static double read_temperature(void *arg)
{
    double temp = read_temperature_real(arg);
    if (!isnan(temp) && g_alert_threshold > 0 && temp >= (double)g_alert_threshold) {
        conexio_cloud_publish_alert("temperature", temp, (double)g_alert_threshold);
        LOG_WRN("Alert: temperature %.1f °C exceeds threshold %d °C",
                temp, g_alert_threshold);
    }
    return temp;
}

static double read_humidity(void *arg)
{
    return read_humidity_real(arg);
}

#endif /* CONFIG_CONEXIO_SAMPLE_SIMULATED_SENSORS */

/* ── Settings handler ─────────────────────────────────────────────────────
 * Receive alertThreshold updates pushed from Conexio Console → OTA Config.
 */
static enum conexio_setting_status on_alert_threshold(int32_t value, void *arg)
{
    ARG_UNUSED(arg);
    g_alert_threshold = (int)value;
    LOG_INF("Setting: alertThreshold → %d °C", g_alert_threshold);
    return CONEXIO_SETTING_OK;
}

/* ── Cloud event handler ────────────────────────────────────────────────── */
static void on_cloud_event(const struct conexio_cloud_event *evt)
{
    switch (evt->type) {
    case CONEXIO_CLOUD_EVT_CONNECTED:
        LOG_INF("Connected — %s | App v%s",
                conexio_cloud_device_id(), APP_VERSION_STRING);
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
    LOG_INF("=== Conexio Sensor Data Sample ===");
    LOG_INF("Interval: %ds | Simulated: %s | SHT4x: %s",
            CONFIG_CONEXIO_CLOUD_INTERVAL_SEC,
            IS_ENABLED(CONFIG_CONEXIO_SAMPLE_SIMULATED_SENSORS) ? "yes" : "no",
            IS_ENABLED(CONFIG_SHT4X) ? "enabled" : "disabled");

    /* Initialise real SHT4x sensor — guarded; no-op on boards without the node */
#if DT_HAS_COMPAT_STATUS_OKAY(sensirion_sht4x)
    s_sht4x = DEVICE_DT_GET_ANY(sensirion_sht4x);
    if (!device_is_ready(s_sht4x)) {
        LOG_WRN("SHT4x not ready — real sensor metrics will be skipped");
        s_sht4x = NULL;
    } else {
        LOG_INF("SHT4x ready — real sensor metrics enabled");
    }
#endif

    /* Register primary callbacks — always published under these names */
    conexio_cloud_register_sensor("temperature", read_temperature, NULL);
    conexio_cloud_register_sensor("humidity",    read_humidity,    NULL);

    /* Register SHT4x-specific names only when simulated mode is active.
     * When SIMULATED_SENSORS=n, "temperature"/"humidity" ARE the real values
     * (forwarded from the SHT4x callbacks), so _real would be redundant
     * duplicates — two separate I2C reads would give slightly different values
     * and confuse dashboards. With SIMULATED_SENSORS=y they are useful for
     * side-by-side comparison of simulated vs. real sensor output. */
#if defined(CONFIG_CONEXIO_SAMPLE_SIMULATED_SENSORS)
    conexio_cloud_register_sensor("temperature_real", read_temperature_real, NULL);
    conexio_cloud_register_sensor("humidity_real",    read_humidity_real,    NULL);
#endif

    /* Register alertThreshold setting — Conexio Console can update remotely */
    conexio_cloud_register_setting_int("alertThreshold", on_alert_threshold, NULL);

    /* Initialise: LTE → NTP → TLS → MQTT thread */
    int ret = conexio_cloud_init(on_cloud_event);
    if (ret) {
        LOG_ERR("init failed (%d)", ret); return -1;
    }

    LOG_INF("Connecting...");
    if (conexio_cloud_wait_connected(60000) == 0) {
        k_sleep(K_SECONDS(2));
        conexio_cloud_publish();
    }

    /* The SDK background thread handles periodic publishing at
     * CONFIG_CONEXIO_CLOUD_INTERVAL_SEC. The main thread just stays alive. */
    while (1) {
        k_sleep(K_SECONDS(CONFIG_CONEXIO_CLOUD_INTERVAL_SEC));
    }
    return 0;
}
