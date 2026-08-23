/*
 * main.c — Conexio Advanced Sample Application
 *
 * nRF Connect SDK v3.2.1 / nRF91xx
 *
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │  This is what an application built on conexio-cloud-sdk looks like.    │
 * │                                                                         │
 * │  The application provides:                                              │
 * │    1. Sensor reading callbacks (registered — SDK calls them)            │
 * │    2. Actuator command handlers (FAN_ON, FAN_OFF)                       │
 * │    3. OTA Config settings handlers (alertThreshold, debugMode)          │
 * │    4. Optional cloud event handler (status LED etc.)                    │
 * │    5. main(): register → init → wait_connected → sleep loop             │
 * │                                                                         │
 * │  The SDK provides everything else — no boilerplate needed:              │
 * │    REBOOT, SET_INTERVAL, FIRMWARE_UPDATE commands — built-in            │
 * │    telemetryIntervalSec setting — built-in                              │
 * │    Retry, WDT, PSM, offline buffer, FOTA — enabled via prj.conf        │
 * │    _rssi, _snr, _reboot_cnt, _battery_mv, _sdk_version — auto-metrics  │
 * │    _app_fw_version — auto-published from app VERSION file               │
 * │    conexio_cloud_register_interval(min, max) — Golioth-style            │
 * └─────────────────────────────────────────────────────────────────────────┘
 */

/*
 * ONE include for everything — the SDK umbrella header automatically
 * includes fota.h, offline_buffer.h, power_mgr.h, retry.h based on Kconfig.
 */
#include <conexio_cloud/conexio_cloud.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/random/random.h>
#include <zephyr/logging/log.h>
#include <math.h>    /* NAN — returned by read_battery_mv on failure */

/* App firmware version from VERSION file — generated at build time */
#if __has_include(<app_version.h>)
#  include <app_version.h>
#else
#  define APP_VERSION_STRING "unknown"
#endif

/* nPM1300 fuel gauge — battery voltage and state-of-charge */
#include "fuel_gauge.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

/* ── Application state ────────────────────────────────────────────────── */
static int  g_alert_threshold = 80;
static bool g_debug_mode      = false;

/* ── LED GPIO ─────────────────────────────────────────────────────────── */
/*
 * The Conexio Stratus Pro has one user-controllable LED:
 *   led0 → gpio0 pin 25, GPIO_ACTIVE_HIGH
 * Defined in the board DTS (conexio_stratus_pro_common.dtsi).
 * Accessible via the led0 alias — no overlay entry needed.
 */
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec g_led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/* ── Telemetry interval limits ────────────────────────────────────────── */
#define TELEMETRY_INTERVAL_MIN_S    10       /* Fastest: 10 s — debug / development   */
#define TELEMETRY_INTERVAL_MAX_S    604800   /* Slowest: 7 days — ultra-low-power      */

/* ── Alert threshold limits ───────────────────────────────────────────── */
#define ALERT_THRESHOLD_MIN         0        /* 0 = disabled                           */
#define ALERT_THRESHOLD_MAX         200      /* Covers full sensor range               */

/* ── nPM1300 fuel gauge device handles ───────────────────────────────── */
/*
 * pmic_charger is the nPM1300 charger sub-device used by the nRF Fuel Gauge
 * library to read voltage, current, temperature, and charge status.
 * Both nodes are defined in conexio_stratus_pro_common.dtsi.
 */
static const struct device *pmic_charger = DEVICE_DT_GET(DT_NODELABEL(pmic_charger));

/* Flag set once fuel_gauge_init() has succeeded — guards read_battery_mv. */
static bool g_fuel_gauge_ready = false;

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

#if defined(CONFIG_CONEXIO_SAMPLE_SIMULATED_SENSORS)
/*
 * ⚠ SIMULATED SENSORS — for development and demonstration only.
 *
 * These callbacks return random values. They are compiled only when
 * CONFIG_CONEXIO_SAMPLE_SIMULATED_SENSORS=y (default in prj.conf).
 *
 * To use real sensors:
 *   1. Set CONFIG_CONEXIO_SAMPLE_SIMULATED_SENSORS=n in prj.conf
 *   2. Replace these stubs with actual Zephyr sensor driver calls
 *
 * A build warning is emitted below as a reminder.
 */
#warning "Simulated sensor data is enabled (CONFIG_CONEXIO_SAMPLE_SIMULATED_SENSORS=y)." \
         " Replace with real sensor reads before deploying to production."

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

#else
/*
 * Real sensor reads — implement these for your hardware.
 * Returning NAN skips the metric for that publish cycle.
 */
static double read_temperature(void *arg)
{
    ARG_UNUSED(arg);
    /* TODO: implement for your sensor hardware
     * Example (BME280 / SHT31 / etc.):
     *   struct sensor_value val;
     *   sensor_sample_fetch(temp_dev);
     *   sensor_channel_get(temp_dev, SENSOR_CHAN_AMBIENT_TEMP, &val);
     *   return sensor_value_to_double(&val);
     */
    return (double)NAN;
}

static double read_humidity(void *arg)
{
    ARG_UNUSED(arg);
    /* TODO: implement for your sensor hardware */
    return (double)NAN;
}
#endif /* CONFIG_CONEXIO_SAMPLE_SIMULATED_SENSORS */

/* ── Battery voltage from nPM1300 fuel gauge ──────────────────────────── */
/*
 * Reads SENSOR_CHAN_GAUGE_VOLTAGE from the nPM1300 pmic_charger node and
 * returns the value in millivolts so the SDK publishes it as _battery_mv.
 *
 * Replaces the modem AT%XVBAT reading (CONFIG_CONEXIO_CLOUD_AUTO_BATTERY=n):
 *   - Higher accuracy: nPM1300 measures actual battery terminal voltage
 *   - Reflects true cell voltage, not the VDDMAIN rail seen by the modem
 *
 * Returns NAN if the fuel gauge has not been initialised or the read fails —
 * the SDK will skip _battery_mv for that publish cycle rather than sending 0.
 */
static double read_battery_mv(void *arg)
{
    ARG_UNUSED(arg);

    if (!g_fuel_gauge_ready) {
        return (double)NAN;
    }

#if defined(CONFIG_CONEXIO_CLOUD_BATTERY_METRICS)
    /* When CONFIG_CONEXIO_CLOUD_BATTERY_METRICS=y the SDK's battery_read_soc()
     * has already called sensor_sample_fetch() and cached the voltage in
     * g_last_battery_mv. Re-use it — no second fetch needed on the same cycle.
     *
     * NOTE: The SDK calls sensor callbacks (this function) BEFORE calling
     * battery_read_soc() in build_payload(). So on the very first publish
     * g_last_battery_mv is NAN and we fall through to a direct read below.
     * On all subsequent publishes the cached value is already fresh. */
    extern double g_last_battery_mv;
    if (!isnan(g_last_battery_mv)) {
        return g_last_battery_mv;
    }
#endif

    /* Direct read — used on first publish (cache not yet populated) or
     * when CONFIG_CONEXIO_CLOUD_BATTERY_METRICS=n. */
    struct sensor_value voltage;

    int ret = sensor_sample_fetch(pmic_charger);
    if (ret < 0) {
        LOG_WRN("fuel gauge: sensor_sample_fetch failed (%d)", ret);
        return (double)NAN;
    }

    ret = sensor_channel_get(pmic_charger, SENSOR_CHAN_GAUGE_VOLTAGE, &voltage);
    if (ret < 0) {
        LOG_WRN("fuel gauge: sensor_channel_get GAUGE_VOLTAGE failed (%d)", ret);
        return (double)NAN;
    }

    /* SENSOR_CHAN_GAUGE_VOLTAGE: val1 = whole Volts, val2 = micro-Volts fraction.
     * Convert to millivolts: (val1 * 1e6 + val2) / 1000 */
    double voltage_mv = ((double)voltage.val1 * 1000.0) +
                        ((double)voltage.val2 / 1000.0);

    LOG_DBG("fuel gauge: battery %.3f V (%d mV)",
            voltage_mv / 1000.0, (int)voltage_mv);

    return voltage_mv;
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

/* ── LED command handlers ─────────────────────────────────────────────── */
/*
 * LED_ON  — turns on the on-board LED (gpio0 pin 25, ACTIVE_HIGH).
 * LED_OFF — turns off the on-board LED.
 *
 * These commands are used to test Device Schedules from the Conexio Console:
 *   1. Create a schedule with command = LED_ON  (start time)
 *   2. Create a schedule with command = LED_OFF (end time, or separate schedule)
 * The Schedules executor publishes the command via MQTT to
 *   devices/{deviceId}/commands  (QoS 1)
 * and the SDK dispatches it here.
 */
static void on_led_on(const char *payload_json, void *arg)
{
    ARG_UNUSED(payload_json); ARG_UNUSED(arg);
    int ret = gpio_pin_set_dt(&g_led, 1);
    if (ret != 0) {
        LOG_ERR("LED_ON: gpio_pin_set_dt failed (%d)", ret);
    } else {
        LOG_INF("LED_ON: LED is ON");
    }
}

static void on_led_off(const char *payload_json, void *arg)
{
    ARG_UNUSED(payload_json); ARG_UNUSED(arg);
    int ret = gpio_pin_set_dt(&g_led, 0);
    if (ret != 0) {
        LOG_ERR("LED_OFF: gpio_pin_set_dt failed (%d)", ret);
    } else {
        LOG_INF("LED_OFF: LED is OFF");
    }
    /* Cancel the firmware watchdog — stop command arrived from the cloud,
     * so the autonomous timer is no longer needed. This prevents a duplicate
     * LED_OFF from firing when the k_timer would have expired. */
    conexio_cloud_cancel_schedule_watchdog();
}

/* ── Schedule lifecycle callback ──────────────────────────────────────── */
/*
 * Called by the SDK for three events:
 *   STARTED  — cloud delivered LED_ON; firmware watchdog timer is now armed.
 *   STOPPED  — LED_OFF was fired by the firmware timer (device was offline).
 *   EXPIRED  — Device rebooted during LED_ON window; LED_OFF ran on boot.
 */
static void on_schedule(const struct conexio_schedule_event *evt)
{
    switch (evt->type) {
    case CONEXIO_SCHEDULE_EVT_STARTED:
        LOG_INF("Schedule watchdog armed — '%s' will fire autonomously at stopAt",
                evt->stop_command);
        break;
    case CONEXIO_SCHEDULE_EVT_STOPPED:
        LOG_INF("Schedule stopped autonomously — '%s' fired by firmware timer",
                evt->stop_command);
        break;
    case CONEXIO_SCHEDULE_EVT_EXPIRED:
        LOG_WRN("Schedule expired on boot — '%s' executed immediately",
                evt->stop_command);
        break;
    default:
        break;
    }
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
        LOG_INF("Connected — %s | App v%s | SDK v" CONEXIO_CLOUD_VERSION,
                conexio_cloud_device_id(), APP_VERSION_STRING);
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
    LOG_INF("=== Conexio Advanced Sample ===");
    LOG_INF("App v%s | SDK v" CONEXIO_CLOUD_VERSION, APP_VERSION_STRING);

    /* ── LED GPIO init ────────────────────────────────────────────────
     * Configure the on-board LED as output, initially OFF.
     * Must happen before conexio_cloud_init() so the LED is ready
     * when the first LED_ON/LED_OFF command arrives.              */
    if (!gpio_is_ready_dt(&g_led)) {
        LOG_ERR("LED GPIO device not ready");
    } else {
        int ret = gpio_pin_configure_dt(&g_led, GPIO_OUTPUT_INACTIVE);
        if (ret != 0) {
            LOG_ERR("LED GPIO configure failed (%d)", ret);
        } else {
            LOG_INF("LED GPIO ready (pin %d)", g_led.pin);
        }
    }

    /* ── Register sensor callbacks ────────────────────────────────────
     * The SDK calls these before each publish — no send_metric in loop. */
    conexio_cloud_register_sensor("temperature", read_temperature, NULL);
    conexio_cloud_register_sensor("humidity",    read_humidity,    NULL);
    /* _battery_mv from nPM1300 fuel gauge — replaces modem AT%XVBAT.
     * Registered with the metric name "_battery_mv" so the SDK publishes
     * it under that exact key rather than double-publishing alongside the
     * auto-battery metric (disabled via CONFIG_CONEXIO_CLOUD_AUTO_BATTERY=n). */
    conexio_cloud_register_sensor("_battery_mv", read_battery_mv, NULL);

    /* ── Register application commands ───────────────────────────────
     * SDK built-ins: REBOOT, SET_INTERVAL, FIRMWARE_UPDATE
     *
     * LED_ON / LED_OFF are used with Device Schedules to test
     * timed command delivery from the Conexio Console.             */
    conexio_cloud_register_command("FAN_ON",  on_fan_on,  NULL);
    conexio_cloud_register_command("FAN_OFF", on_fan_off, NULL);
    conexio_cloud_register_command("LED_ON",  on_led_on,  NULL);
    conexio_cloud_register_command("LED_OFF", on_led_off, NULL);

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
     * The SDK validates any SET_INTERVAL or telemetryIntervalSec OTA
     * Config push against these limits — no range check in your code.
     * Values must be within CONFIG_CONEXIO_CLOUD_INTERVAL_SEC range.  */
    conexio_cloud_register_interval(TELEMETRY_INTERVAL_MIN_S,
                                    TELEMETRY_INTERVAL_MAX_S);

    /* ── Register schedule lifecycle callback ─────────────────────────
     * Called when the firmware watchdog arms, fires, or runs on boot.
     * The SDK stores the stop command + stopAt in NVS so LED_OFF runs
     * even if the device loses connectivity after receiving LED_ON.   */
    conexio_cloud_register_schedule_cb(on_schedule);

    /* ── nPM1300 fuel gauge init ──────────────────────────────────────────
     * Initialises the nRF Fuel Gauge library with battery model and initial
     * readings. Must happen before conexio_cloud_init() so read_battery_mv()
     * is ready when the SDK background thread calls it on the first publish. */
    if (!device_is_ready(pmic_charger)) {
        LOG_ERR("pmic_charger device not ready — battery voltage unavailable");
    } else if (fuel_gauge_init(pmic_charger) < 0) {
        LOG_ERR("fuel_gauge_init failed — battery voltage unavailable");
    } else {
        g_fuel_gauge_ready = true;
        LOG_INF("nPM1300 fuel gauge initialised");
    }

    /* ── Single SDK init — handles everything ─────────────────────────
     * LTE → NTP → PSM decision → config fetch → transport init →
     * FOTA check → cloud thread spawn                                 */
    int ret = conexio_cloud_init(cloud_event_handler);
    if (ret) {
        LOG_ERR("conexio_cloud_init failed (%d)", ret);
        return -1;
    }

    /* ── Wait for MQTT connection then do boot publish ────────────────
     * conexio_cloud_wait_connected() blocks until MQTT CONNACK arrives
     * (or times out). Replaces the manual K_SEM_DEFINE boilerplate.
     * Boot-once metrics (_reboot_reason, _modem_fw, etc.) are included
     * in this first publish.                                           */
    LOG_INF("Waiting for MQTT connection...");
    if (conexio_cloud_wait_connected(60000) == 0) {
        LOG_INF("Boot publish — sending telemetry immediately after reset");
        int pub_ret = conexio_cloud_publish();
        if (pub_ret) {
            LOG_WRN("Boot publish failed (%d) — will retry at next interval", pub_ret);
        }
    } else {
        LOG_WRN("MQTT connect timeout — skipping boot publish");
    }

    /* ── Main loop ────────────────────────────────────────────────────
     * The SDK background thread handles publishing, PSM, offline
     * buffering, reconnects, and FOTA. This loop just keeps main()
     * alive and wakes periodically to allow the interval to change.
     *
     * Uses a short 5-second poll so SET_INTERVAL or telemetryIntervalSec
     * OTA Config changes take effect within 5s instead of waiting out
     * the full current interval (which could be hours for low-power
     * devices). The background thread does the actual rate control —
     * this sleep is just to keep main() from spinning.               */
    while (1) {
        k_sleep(K_SECONDS(5));
    }

    return 0;
}
