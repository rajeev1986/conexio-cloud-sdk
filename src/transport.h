/**
 * transport.h — Internal transport interface (Phase 2, private to SDK)
 *
 * Topic architecture (versioned, split by data type):
 *
 *   D2C publishes:
 *     v1/devices/{id}/telemetry    — app sensor data (temperature, humidity, …)
 *     v1/devices/{id}/diagnostics  — SDK system metrics (_rssi, _reboot, _lte, …)
 *     v1/devices/{id}/location     — cellular location fixes (_loc_*)
 *     v1/devices/{id}/logs         — log stream entries (_log array)
 *     v1/devices/{id}/alerts       — app-triggered threshold alerts
 *     v1/devices/{id}/commands/ack — command execution acknowledgement
 *     v1/devices/{id}/config/ack   — config push application acknowledgement
 *
 *   C2D subscriptions:
 *     v1/devices/{id}/commands     — commands from Conexio Console
 *     v1/devices/{id}/config       — OTA Config pushes
 *
 *   FOTA (AWS IoT Jobs API — format fixed by AWS, no version prefix):
 *     $aws/things/{id}/jobs/notify
 *     $aws/things/{id}/jobs/{jobId}/update
 */

#ifndef CONEXIO_TRANSPORT_H
#define CONEXIO_TRANSPORT_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "config_fetch.h"

/* ── Topic category constants ─────────────────────────────────────────────
 * Used as the 'category' field on metric_entry to route each metric to
 * the correct versioned MQTT topic at publish time.
 */
#define TOPIC_CAT_TELEMETRY    'a'   /* app sensor data   → v1/.../telemetry   */
#define TOPIC_CAT_DIAGNOSTICS  'd'   /* SDK system health → v1/.../diagnostics */
#define TOPIC_CAT_LOCATION     'l'   /* _loc_* metrics    → v1/.../location    */
#define TOPIC_CAT_LOGS         'g'   /* _log entries      → v1/.../logs        */

/* ── Core transport interface ─────────────────────────────────────────────*/

/** Initialise transport using runtime-fetched config (host, API key, etc.) */
int  transport_init_with_config(const char *device_id,
                                const struct conexio_cloud_config_t *cfg);
int  transport_connect(void);
int  transport_disconnect(void);

/**
 * @brief Publish a payload to the telemetry topic (backwards-compatible).
 * Equivalent to transport_publish_to(TOPIC_CAT_TELEMETRY, payload, len).
 */
int  transport_publish(const char *payload, size_t len);

/**
 * @brief Publish a payload to the topic corresponding to the given category.
 *
 * @param category  One of TOPIC_CAT_TELEMETRY / DIAGNOSTICS / LOCATION / LOGS.
 * @param payload   JSON string to publish.
 * @param len       Length of payload in bytes.
 * @return 0 on success, -ENOTCONN if not connected, mqtt error otherwise.
 */
int  transport_publish_to(char category, const char *payload, size_t len);

/**
 * @brief Publish an alert to the dedicated alerts topic.
 *
 * @param payload   JSON alert payload.
 * @param len       Length in bytes.
 * @return 0 on success, -ENOTCONN if not connected.
 */
int  transport_publish_alert(const char *payload, size_t len);

/**
 * @brief Publish to an arbitrary MQTT topic (used by fota.c for AWS IoT Jobs).
 */
int  transport_publish_raw(const char *topic, const char *payload, size_t len);

/**
 * @brief Queue a command or config ACK for reliable delivery.
 *
 * Stores the ACK in a small RAM ring buffer. The transport layer retries
 * delivery on each poll cycle until acknowledged (up to
 * CONFIG_CONEXIO_CLOUD_ACK_RETRY_MAX attempts). This prevents the
 * "delivered forever" dashboard state when the connection drops between
 * PUBACK and ACK publish.
 *
 * @param is_config  true = config/ack topic, false = commands/ack topic.
 * @param payload    ACK JSON string (copied internally).
 * @param len        Length of payload.
 */
void transport_queue_ack(bool is_config, const char *payload, size_t len);

/**
 * @brief Drain pending ACKs — call from the SDK poll loop.
 * Retries any queued ACKs that have not yet been delivered.
 */
void transport_drain_ack_queue(void);

void transport_poll(k_timeout_t timeout);
bool transport_is_connected(void);

/** Send a config ACK after processing all settings in a push.
 *  Internally calls transport_queue_ack(true, ...) for reliable delivery.
 *  success=true  → all settings applied    → dashboard shows 'applied'
 *  success=false → one or more rejected    → dashboard shows 'failed'
 */
void transport_config_ack(const char *config_id, bool success);

void transport_on_connected(void);
void transport_on_disconnected(void);
void transport_on_message(const char *json_str, size_t len);

#endif /* CONEXIO_TRANSPORT_H */
