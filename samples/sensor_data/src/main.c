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
 * By default this sample uses SIMULATED sensors that return random values.
 * To use real hardware:
 *   1. Set CONFIG_CONEXIO_SAMPLE_SIMULATED_SENSORS=n in prj.conf
 *   2. Implement the real sensor reads in read_temperature() / read_humidity()
 *      using the standard Zephyr sensor driver API.
 *
 * The SDK handles:
 *   - LTE, MQTT, CBOR encoding, retry, PSM
 *   - SDK auto-metrics: _rssi, _snr, _reboot_cnt, _sdk_version, etc.
 *
 * The application provides:
 *   - "temperature" (°C) and "humidity" (%) sensor callbacks
 *   - Alert publishing when temperature exceeds a threshold
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

/* ── Sensor callbacks ─────────────────────────────────────────────────────
 * The SDK calls these before every publish.
 * Return NAN to skip a reading for a given cycle.
 */

#if defined(CONFIG_CONEXIO_SAMPLE_SIMULATED_SENSORS)
#warning "Simulated sensors active — replace with real driver reads for production."

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

#else

static double read_temperature(void *arg)
{
    ARG_UNUSED(arg);
    /*
     * Replace with your sensor driver:
     *   struct sensor_value val;
     *   sensor_sample_fetch(my_sensor);
     *   sensor_channel_get(my_sensor, SENSOR_CHAN_AMBIENT_TEMP, &val);
     *   double temp = sensor_value_to_double(&val);
     *   if (g_alert_threshold > 0 && temp >= (double)g_alert_threshold)
     *       conexio_cloud_publish_alert("temperature", temp, g_alert_threshold);
     *   return temp;
     */
    return (double)NAN;
}

static double read_humidity(void *arg)
{
    ARG_UNUSED(arg);
    /* Replace with your sensor driver */
    return (double)NAN;
}
#endif

/* ── Real SHT4x sensor callbacks ────────────────────────────────────────
 * read_temperature_real: fetches a fresh sample from the SHT4x, returns
 *   temperature in °C as a double.
 * read_humidity_real: reads the humidity channel from the same buffered
 *   sample WITHOUT triggering a second I2C transaction (Zephyr sensor API
 *   stores the last fetch result until the next call to sensor_sample_fetch).
 *
 * IMPORTANT: register temperature_real BEFORE humidity_real so that the
 *   SDK always calls the fetch before the channel-only read in the same
 *   publish cycle.
 *
 * Returns NAN when the sensor is absent, not ready, or fetch fails —
 * the SDK skips NAN values silently so the sample works on boards
 * without the sensor attached.
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
    sensor_channel_get(s_sht4x, SENSOR_CHAN_AMBIENT_TEMP, &val);
    return sensor_value_to_double(&val);
}

static double read_humidity_real(void *arg)
{
    ARG_UNUSED(arg);
    if (s_sht4x == NULL || !device_is_ready(s_sht4x)) {
        return (double)NAN;
    }
    /* No sensor_sample_fetch here — read_temperature_real (registered and
     * called first) already fetched the sample this cycle.
     * sensor_channel_get reads from the driver's internal buffer. */
    struct sensor_value val;
    sensor_channel_get(s_sht4x, SENSOR_CHAN_HUMIDITY, &val);
    return sensor_value_to_double(&val);
}

#else  /* !DT_HAS_COMPAT_STATUS_OKAY(sensirion_sht4x) */

static double read_temperature_real(void *arg) { ARG_UNUSED(arg); return (double)NAN; }
static double read_humidity_real(void *arg)    { ARG_UNUSED(arg); return (double)NAN; }

#endif /* DT_HAS_COMPAT_STATUS_OKAY(sensirion_sht4x) */

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

    /* Register sensor callbacks — SDK calls these before each publish */
    conexio_cloud_register_sensor("temperature", read_temperature, NULL);
    conexio_cloud_register_sensor("humidity",    read_humidity,    NULL);

    /* Real SHT4x metrics — registered after simulated; NAN returned when absent */
    conexio_cloud_register_sensor("temperature_real", read_temperature_real, NULL);
    conexio_cloud_register_sensor("humidity_real",    read_humidity_real,    NULL);

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

    while (1) {
        k_sleep(K_SECONDS(5));
    }
    return 0;
}
