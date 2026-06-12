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

/** Register a handler for a boolean setting. */
int conexio_cloud_register_setting_bool(const char *key,
                                        conexio_bool_setting_cb_t handler,
                                        void *arg);

/** Register a handler for a float setting. */
int conexio_cloud_register_setting_float(const char *key,
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

/**
 * @brief Returns the current telemetry publish interval in seconds.
 *
 * This may differ from CONFIG_CONEXIO_CLOUD_INTERVAL_SEC if the interval
 * was updated at runtime via the SET_INTERVAL command or the
 * telemetryIntervalSec OTA Config setting.
 */
int conexio_cloud_get_interval_sec(void);

/** Returns the SDK semantic version string, e.g. "2.1.0". */
const char *conexio_cloud_version(void);

/**
 * @brief Override the default FOTA event callback.
 * Only available when CONFIG_CONEXIO_CLOUD_FOTA=y.
 */
#if defined(CONFIG_CONEXIO_CLOUD_FOTA)
#include <fota.h>
void conexio_cloud_set_fota_cb(fota_event_cb_t cb);
#endif

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
