/**
 * @file conexio_cloud.h
 * @brief Conexio Cloud SDK — public API (umbrella header)
 *
 * This is the ONLY header an application needs to include.
 * It automatically includes SDK feature headers based on Kconfig:
 *   - fota.h           when CONFIG_CONEXIO_CLOUD_FOTA=y
 *   - offline_buffer.h when CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER=y
 *   - power_mgr.h      when CONFIG_CONEXIO_CLOUD_PSM=y
 *   - retry.h          when CONFIG_CONEXIO_CLOUD_RETRY=y
 *
 * Usage — the minimal application:
 * @code
 *   #include <conexio_cloud/conexio_cloud.h>
 *
 *   static double read_temp(void *arg) { return sensor_read_temperature(); }
 *   static double read_hum(void *arg)  { return sensor_read_humidity(); }
 *
 *   void main(void) {
 *       conexio_cloud_register_sensor("temperature", read_temp, NULL);
 *       conexio_cloud_register_sensor("humidity",    read_hum,  NULL);
 *       conexio_cloud_register_command("FAN_ON", on_fan_on, NULL);
 *       conexio_cloud_register_setting_int("alertThreshold", on_threshold, NULL);
 *       conexio_cloud_register_interval(10, 7200);
 *       conexio_cloud_init(NULL);  // everything else is handled by the SDK
 *       while (1) { k_sleep(K_SECONDS(conexio_cloud_get_interval_sec())); }
 *   }
 * @endcode
 */

#ifndef CONEXIO_CLOUD_H
#define CONEXIO_CLOUD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Event types ─────────────────────────────────────────────────────────── */

/**
 * Event types delivered to your application event callback.
 *
 * NOTE: Commands and settings are no longer delivered through events.
 *       Register dedicated handlers using:
 *         conexio_cloud_register_command()
 *         conexio_cloud_register_setting_*()
 *
 * The event callback is now only needed for connection lifecycle events.
 */
enum conexio_cloud_event_type {
    /** Cloud connection established — telemetry can now be sent. */
    CONEXIO_CLOUD_EVT_CONNECTED,

    /** Cloud connection lost — SDK will reconnect automatically. */
    CONEXIO_CLOUD_EVT_DISCONNECTED,

    /** A telemetry publish completed successfully. */
    CONEXIO_CLOUD_EVT_PUBLISHED,

    /** An error occurred — check evt->error for the errno value. */
    CONEXIO_CLOUD_EVT_ERROR,
};

/** Event structure passed to your application event callback. */
struct conexio_cloud_event {
    enum conexio_cloud_event_type type;
    union {
        int error; /**< Valid when type == CONEXIO_CLOUD_EVT_ERROR. */
    } data;
};

/** Application event callback — called for connection lifecycle events. */
typedef void (*conexio_cloud_event_cb_t)(const struct conexio_cloud_event *evt);

/* ── Command handler type ────────────────────────────────────────────────── */

/**
 * @brief Callback type for a registered command handler.
 *
 * Called by the SDK when a matching command arrives from the dashboard.
 *
 * @param payload_json  Raw JSON payload string from the command, or NULL.
 *                      Parse with cJSON if you need individual fields.
 *                      e.g. for FAN_ON: "{\"speed\":80}"
 * @param arg           User argument supplied at registration.
 */
typedef void (*conexio_command_handler_t)(const char *payload_json, void *arg);

/* ── Setting status codes (returned by setting handlers) ─────────────────── */

/** Return codes from setting handler callbacks. */
enum conexio_setting_status {
    /** Setting applied successfully. */
    CONEXIO_SETTING_OK = 0,
    /** Setting key not recognised by this device. */
    CONEXIO_SETTING_KEY_UNKNOWN = 1,
    /** Setting value is outside the allowed range. */
    CONEXIO_SETTING_VALUE_OUT_OF_RANGE = 2,
    /** Setting value has the wrong type. */
    CONEXIO_SETTING_VALUE_WRONG_TYPE = 3,
    /** General error applying the setting. */
    CONEXIO_SETTING_ERROR = 4,
};

/* ── Setting handler types ───────────────────────────────────────────────── */

/**
 * @brief Callback for an integer setting received from the OTA Config page.
 *
 * @param value  The new integer value.
 * @param arg    User argument supplied at registration.
 * @return       CONEXIO_SETTING_OK on success, or an error code.
 */
typedef enum conexio_setting_status (*conexio_int_setting_cb_t)(int32_t value, void *arg);

/**
 * @brief Callback for a boolean setting.
 */
typedef enum conexio_setting_status (*conexio_bool_setting_cb_t)(bool value, void *arg);

/**
 * @brief Callback for a float setting.
 */
typedef enum conexio_setting_status (*conexio_float_setting_cb_t)(float value, void *arg);

/**
 * @brief Callback for a string setting.
 */
typedef enum conexio_setting_status (*conexio_string_setting_cb_t)(const char *value,
                                                                    size_t len,
                                                                    void *arg);

/* ── Initialisation ──────────────────────────────────────────────────────── */

/**
 * @brief Initialise the Conexio Cloud SDK.
 *
 * Call AFTER registering all command and setting handlers.
 *
 * @param cb  Event callback for CONNECTED/DISCONNECTED/PUBLISHED/ERROR.
 *            Can be NULL if you don't need lifecycle events.
 * @return    0 on success, negative errno on failure.
 */
int conexio_cloud_init(conexio_cloud_event_cb_t cb);

/** Manually trigger a connection (for apps managing LTE independently). */
int conexio_cloud_connect(void);

/** Disconnect from the cloud. */
int conexio_cloud_disconnect(void);

/* ── Command registration ────────────────────────────────────────────────── */

/**
 * @brief Register a handler for a named command from the dashboard.
 *
 * Must be called before conexio_cloud_init().
 *
 * Example:
 * @code
 * static void on_fan_on(const char *payload_json, void *arg)
 * {
 *     int speed = 100;
 *     if (payload_json) {
 *         cJSON *p = cJSON_Parse(payload_json);
 *         const cJSON *s = cJSON_GetObjectItem(p, "speed");
 *         if (cJSON_IsNumber(s)) speed = (int)s->valuedouble;
 *         cJSON_Delete(p);
 *     }
 *     fan_set_speed(speed);
 * }
 *
 * conexio_cloud_register_command("FAN_ON", on_fan_on, NULL);
 * @endcode
 *
 * @param name     Command name, e.g. "FAN_ON". Case-sensitive.
 * @param handler  Handler function. Must not be NULL.
 * @param arg      Optional user argument forwarded to handler. Can be NULL.
 * @return 0 on success, -ENOMEM if command registry is full.
 */
int conexio_cloud_register_command(const char *name,
                                   conexio_command_handler_t handler,
                                   void *arg);

/* ── Settings registration ───────────────────────────────────────────────── */

/**
 * @brief Register a handler for an integer setting from the OTA Config page.
 *
 * When the dashboard pushes a config object containing this key with an
 * integer value, your handler is called with the new value.
 *
 * Example:
 * @code
 * static enum conexio_setting_status on_interval(int32_t value, void *arg)
 * {
 *     if (value < 10 || value > 3600) return CONEXIO_SETTING_VALUE_OUT_OF_RANGE;
 *     g_telemetry_interval_sec = value;
 *     return CONEXIO_SETTING_OK;
 * }
 *
 * conexio_cloud_register_setting_int("telemetryIntervalSec", on_interval, NULL);
 * @endcode
 *
 * @param key      Setting key name as it appears in the config JSON.
 * @param handler  Handler callback. Must not be NULL.
 * @param arg      Optional user argument. Can be NULL.
 * @return 0 on success, -ENOMEM if settings registry is full.
 */
int conexio_cloud_register_setting_int(const char *key,
                                       conexio_int_setting_cb_t handler,
                                       void *arg);

/**
 * @brief Register an integer setting with SDK-enforced min/max range.
 *
 * Golioth-style: declare the valid range as named constants. The SDK
 * validates the incoming value before calling your handler — no
 * range check needed in the callback.
 *
 * Example:
 * @code
 * #define ALERT_THRESHOLD_MIN   0
 * #define ALERT_THRESHOLD_MAX   200
 *
 * static enum conexio_setting_status on_alert_threshold(int32_t value, void *arg)
 * {
 *     g_alert_threshold = (int)value;   // SDK already validated
 *     LOG_INF("alertThreshold → %d", g_alert_threshold);
 *     return CONEXIO_SETTING_OK;
 * }
 *
 * conexio_cloud_register_setting_int_with_range("alertThreshold",
 *     ALERT_THRESHOLD_MIN, ALERT_THRESHOLD_MAX,
 *     on_alert_threshold, NULL);
 * @endcode
 *
 * @param key      Setting key name as it appears in the config JSON.
 * @param min      Minimum accepted value (inclusive).
 * @param max      Maximum accepted value (inclusive, must be >= min).
 * @param handler  Handler callback. Must not be NULL.
 * @param arg      Optional user argument. Can be NULL.
 * @return 0 on success, -ENOMEM if settings registry is full.
 */
int conexio_cloud_register_setting_int_with_range(const char *key,
                                                  int32_t min, int32_t max,
                                                  conexio_int_setting_cb_t handler,
                                                  void *arg);

/** Register a handler for a boolean setting. */
int conexio_cloud_register_setting_bool(const char *key,
                                        conexio_bool_setting_cb_t handler,
                                        void *arg);

/** Register a handler for a float setting. */
int conexio_cloud_register_setting_float(const char *key,
                                         conexio_float_setting_cb_t handler,
                                         void *arg);

/**
 * @brief Register a float setting with SDK-enforced min/max range.
 *
 * Example:
 * @code
 * #define SAMPLE_RATE_MIN_HZ   0.1f
 * #define SAMPLE_RATE_MAX_HZ   100.0f
 *
 * static enum conexio_setting_status on_sample_rate(float value, void *arg)
 * {
 *     g_sample_rate_hz = value;   // SDK already validated
 *     LOG_INF("sampleRate → %.2f Hz", (double)g_sample_rate_hz);
 *     return CONEXIO_SETTING_OK;
 * }
 *
 * conexio_cloud_register_setting_float_with_range("sampleRate",
 *     SAMPLE_RATE_MIN_HZ, SAMPLE_RATE_MAX_HZ,
 *     on_sample_rate, NULL);
 * @endcode
 *
 * @param key      Setting key name as it appears in the config JSON.
 * @param min      Minimum accepted value (inclusive).
 * @param max      Maximum accepted value (inclusive, must be >= min).
 * @param handler  Handler callback. Must not be NULL.
 * @param arg      Optional user argument. Can be NULL.
 * @return 0 on success, -ENOMEM if settings registry is full.
 */
int conexio_cloud_register_setting_float_with_range(const char *key,
                                                    float min, float max,
                                                    conexio_float_setting_cb_t handler,
                                                    void *arg);

/** Register a handler for a string setting. */
int conexio_cloud_register_setting_string(const char *key,
                                          conexio_string_setting_cb_t handler,
                                          void *arg);

/* ── Telemetry ───────────────────────────────────────────────────────────── */

/** Queue a numeric metric. Overwrites if same name already queued. */
int conexio_cloud_send_metric(const char *name, double value);

/** Queue a string metric. */
int conexio_cloud_send_metric_str(const char *name, const char *value);

/** Queue a boolean metric. */
int conexio_cloud_send_metric_bool(const char *name, bool value);

/** Immediately publish all queued metrics. */
int conexio_cloud_publish(void);

/* ── Sensor registration (alternative to send_metric in a loop) ──────────── */

/**
 * @brief Callback type for a registered sensor reading.
 *
 * Called by the SDK background thread before each telemetry publish.
 * Return the sensor value as a double.  Return NAN to skip this reading
 * for the current cycle (e.g. sensor temporarily unavailable).
 *
 * @param arg  Optional user argument supplied at registration.
 * @return     Sensor reading, or NAN to skip.
 */
typedef double (*conexio_sensor_read_cb_t)(void *arg);

/**
 * @brief Register a sensor reading callback.
 *
 * The SDK calls this function automatically before each publish and adds the
 * returned value as a metric named `name`.  Applications do not need to call
 * conexio_cloud_send_metric() in their main loop for registered sensors.
 *
 * Must be called before conexio_cloud_init().
 *
 * Example:
 * @code
 * static double read_temp(void *arg) { return sensor_read_temperature(); }
 * static double read_hum(void *arg)  { return sensor_read_humidity(); }
 *
 * conexio_cloud_register_sensor("temperature", read_temp, NULL);
 * conexio_cloud_register_sensor("humidity",    read_hum,  NULL);
 * @endcode
 *
 * @param name      Metric name as it appears in the dashboard.
 * @param callback  Reading function. Return NAN to skip a cycle.
 * @param arg       Optional user argument. Can be NULL.
 * @return 0 on success, -ENOMEM if sensor registry is full.
 */
int conexio_cloud_register_sensor(const char *name,
                                   conexio_sensor_read_cb_t callback,
                                   void *arg);

/* ── Status ──────────────────────────────────────────────────────────────── */

/** Returns true if currently connected to the cloud. */
bool conexio_cloud_is_connected(void);

/** Returns the device ID (bare 15-digit IMEI, e.g. "351358815179730"). Available after conexio_cloud_init(). */
const char *conexio_cloud_device_id(void);

/** Returns the SDK semantic version string, e.g. "2.1.0". */
const char *conexio_cloud_version(void);

/**
 * @brief Compile-time SDK version string.
 *
 * Available before conexio_cloud_init() — use this for early log lines
 * such as the boot banner. Identical to conexio_cloud_version() at runtime.
 *
 * Example:
 * @code
 * LOG_INF("App v%s | Conexio SDK v" CONEXIO_CLOUD_VERSION, APP_VERSION_STRING);
 * @endcode
 */
#define CONEXIO_CLOUD_VERSION "2.1.0"

/* ── SDK status ──────────────────────────────────────────────────────────── */

/**
 * @brief Conexio Cloud SDK connection status.
 *
 * Returned by conexio_cloud_get_status().
 */
enum conexio_cloud_status {
    /** SDK not yet initialised. */
    CONEXIO_CLOUD_STATUS_INIT,
    /** LTE registered; MQTT connection in progress. */
    CONEXIO_CLOUD_STATUS_CONNECTING,
    /** MQTT connected; telemetry can be sent. */
    CONEXIO_CLOUD_STATUS_CONNECTED,
    /** Connection lost; SDK is retrying. */
    CONEXIO_CLOUD_STATUS_OFFLINE,
};

/**
 * @brief Returns the current SDK connection status.
 *
 * More granular than conexio_cloud_is_connected() — useful for status LEDs,
 * displays, or conditional logic that needs to distinguish "not yet started"
 * from "lost connection".
 *
 * @return Current status enum value.
 */
enum conexio_cloud_status conexio_cloud_get_status(void);

/**
 * @brief Block until the SDK is connected to the cloud (MQTT CONNACK received).
 *
 * Eliminates the boilerplate semaphore pattern from main.c:
 * @code
 * // Before (boilerplate in every app):
 * static K_SEM_DEFINE(connected_sem, 0, 1);
 * // ... give semaphore in CONNECTED event handler ...
 * k_sem_take(&connected_sem, K_SECONDS(60));
 *
 * // After (one line):
 * conexio_cloud_wait_connected(60000);
 * @endcode
 *
 * @param timeout_ms  Maximum milliseconds to wait. Use K_FOREVER for no timeout.
 * @return 0 when connected, -ETIMEDOUT if timeout elapsed before connection.
 */
int conexio_cloud_wait_connected(int32_t timeout_ms);

/**
 * @brief Returns the current telemetry publish interval in seconds.
 *
 * This may differ from CONFIG_CONEXIO_CLOUD_INTERVAL_SEC if the interval
 * was updated at runtime via the SET_INTERVAL command or the
 * telemetryIntervalSec OTA Config setting.
 */
int conexio_cloud_get_interval_sec(void);

/**
 * @brief Register the telemetry interval setting with min/max limits.
 *
 * This is the Golioth-style API for configuring SET_INTERVAL.
 * Define the valid range as named constants, register once before init,
 * and the SDK handles everything — range validation, applying the new
 * value, and logging.
 *
 * Must be called before conexio_cloud_init().
 *
 * Example:
 * @code
 * #define TELEMETRY_INTERVAL_MIN_S   10      // fastest: 10 seconds
 * #define TELEMETRY_INTERVAL_MAX_S   7200    // slowest: 2 hours
 *
 * conexio_cloud_register_interval(TELEMETRY_INTERVAL_MIN_S,
 *                                 TELEMETRY_INTERVAL_MAX_S);
 * @endcode
 *
 * @param min_sec   Minimum accepted interval in seconds (>= 1).
 * @param max_sec   Maximum accepted interval in seconds (>= min_sec).
 */
void conexio_cloud_register_interval(int min_sec, int max_sec);

/**
 * @brief Override the default FOTA event callback.
 * Only available when CONFIG_CONEXIO_CLOUD_FOTA=y.
 */
#if defined(CONFIG_CONEXIO_CLOUD_FOTA)
#include <fota.h>
void conexio_cloud_set_fota_cb(fota_event_cb_t cb);
#endif

/* ── Schedule watchdog ───────────────────────────────────────────────────── */

/**
 * Schedule lifecycle event types delivered to the schedule callback.
 */
enum conexio_schedule_event_type {
    /** Start command received from cloud; firmware watchdog timer armed. */
    CONEXIO_SCHEDULE_EVT_STARTED,
    /** Stop command executed autonomously (k_timer fired — no cloud needed). */
    CONEXIO_SCHEDULE_EVT_STOPPED,
    /** stopAt was already in the past on boot; stop command ran immediately. */
    CONEXIO_SCHEDULE_EVT_EXPIRED,
};

/** Event structure passed to the schedule lifecycle callback. */
struct conexio_schedule_event {
    enum conexio_schedule_event_type type;
    /** Name of the stop command (e.g. "LED_OFF"). */
    const char *stop_command;
};

/** Callback type for schedule lifecycle events. */
typedef void (*conexio_schedule_cb_t)(const struct conexio_schedule_event *evt);

/**
 * @brief Register a schedule lifecycle callback.
 *
 * Called by the SDK for three events:
 *   STARTED  — cloud delivered the start command; watchdog timer is now armed.
 *   STOPPED  — stop command fired autonomously by the firmware timer.
 *   EXPIRED  — device rebooted during schedule window; stop ran immediately on boot.
 *
 * Must be called before conexio_cloud_init().
 *
 * Example:
 * @code
 * static void on_schedule(const struct conexio_schedule_event *evt)
 * {
 *     switch (evt->type) {
 *     case CONEXIO_SCHEDULE_EVT_STARTED:
 *         LOG_INF("Schedule started — stop '%s' will fire autonomously",
 *                 evt->stop_command);
 *         break;
 *     case CONEXIO_SCHEDULE_EVT_STOPPED:
 *         LOG_INF("Schedule stopped autonomously ('%s' fired by timer)",
 *                 evt->stop_command);
 *         break;
 *     case CONEXIO_SCHEDULE_EVT_EXPIRED:
 *         LOG_WRN("Schedule expired on boot — '%s' ran immediately",
 *                 evt->stop_command);
 *         break;
 *     }
 * }
 *
 * conexio_cloud_register_schedule_cb(on_schedule);
 * @endcode
 *
 * @param cb  Schedule lifecycle callback. Must not be NULL.
 */
void conexio_cloud_register_schedule_cb(conexio_schedule_cb_t cb);

/**
 * @brief Cancel the active schedule watchdog timer.
 *
 * Call this from the stop command handler (e.g. on_led_off) when the stop
 * command arrives from the cloud before the timer fires — this prevents the
 * watchdog from dispatching a duplicate stop command later.
 *
 * Safe to call when no watchdog is armed (no-op).
 *
 * Example — in the stop command handler:
 * @code
 * static void on_led_off(const char *payload_json, void *arg)
 * {
 *     gpio_pin_set_dt(&g_led, 0);
 *     // Cloud delivered the stop command — cancel the firmware watchdog
 *     // so it doesn't fire a duplicate LED_OFF when the timer expires.
 *     conexio_cloud_cancel_schedule_watchdog();
 *     LOG_INF("LED_OFF");
 * }
 * @endcode
 */
void conexio_cloud_cancel_schedule_watchdog(void);

#ifdef __cplusplus
}
#endif

/* ── Umbrella includes — SDK feature headers pulled in automatically ──────── */
#if defined(CONFIG_CONEXIO_CLOUD_FOTA)
#include <fota.h>
#endif
#if defined(CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER)
#include <offline_buffer.h>
#endif
#if defined(CONFIG_CONEXIO_CLOUD_PSM)
#include <power_mgr.h>
#endif
#if defined(CONFIG_CONEXIO_CLOUD_RETRY)
#include <retry.h>
#endif

#endif /* CONEXIO_CLOUD_H */
