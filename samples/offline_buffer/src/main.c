/*
 * offline_buffer/src/main.c — Offline telemetry buffering sample.
 *
 * Demonstrates how the Conexio Cloud SDK automatically buffers telemetry
 * payloads in NVS flash when the device loses LTE/MQTT connectivity, then
 * replays them to the cloud in chronological order on reconnect.
 *
 * How it works:
 *   - On every publish cycle, the SDK attempts to send telemetry via MQTT
 *   - If the connection is lost, the payload is written to an NVS ring buffer
 *   - On reconnect, the SDK replays buffered payloads (CONFIG_OFFLINE_REPLAY_BATCH
 *     per session) before resuming live telemetry
 *   - Payloads older than CONFIG_OFFLINE_BUFFER_TTL_SEC are silently discarded
 *     during replay to prevent stale data polluting the dashboard timeline
 *
 * This sample logs buffer status to help you observe the behaviour:
 *   - When offline: "Buffered: N/50 payloads in flash"
 *   - On reconnect: "Replaying N buffered payloads"
 *
 * Testing tip: block LTE connectivity (e.g. place in a shielded enclosure
 * or set CONFIG_CONEXIO_CLOUD_LTE_TIMEOUT_SEC=5 to force early timeout) and
 * observe buffer entries accumulating, then restore connectivity and watch
 * the replay sequence.
 *
 * Buffer parameters (set in prj.conf):
 *   CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER_SIZE=50    — max entries in ring buffer
 *   CONFIG_CONEXIO_CLOUD_OFFLINE_REPLAY_BATCH=5    — entries replayed per reconnect
 *   CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER_TTL_SEC=86400 — discard after 24 hours
 */

#include <conexio_cloud/conexio_cloud.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/logging/log.h>

#if __has_include(<app_version.h>)
#  include <app_version.h>
#else
#  define APP_VERSION_STRING "1.0.0"
#endif

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

/* ── Sensor callback ─────────────────────────────────────────────────── */
static double read_temperature(void *arg)
{
    ARG_UNUSED(arg);
    return 20.0 + (double)(sys_rand32_get() % 200) * 0.1;
}

/* ── Buffer-full callback (optional) ─────────────────────────────────────
 * Called when the ring buffer is full and the oldest entry is being dropped.
 * Useful for alerting, LED indicators, or stopping data collection.
 * Enable in prj.conf: CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER_FULL_CB=y
 */
#if defined(CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER_FULL_CB)
static void on_buffer_full(void)
{
    LOG_WRN("Offline buffer full — oldest entry dropped. "
            "Consider increasing CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER_SIZE.");
}
#endif

/* ── Cloud event handler ────────────────────────────────────────────────── */
static void on_cloud_event(const struct conexio_cloud_event *evt)
{
    switch (evt->type) {
    case CONEXIO_CLOUD_EVT_CONNECTED:
        LOG_INF("Connected — %s | App v%s",
                conexio_cloud_device_id(), APP_VERSION_STRING);
        /* Buffer count after reconnect is logged by the SDK internally */
        break;
    case CONEXIO_CLOUD_EVT_DISCONNECTED:
        LOG_WRN("Disconnected — telemetry will buffer to flash");
        break;
    case CONEXIO_CLOUD_EVT_PUBLISHED:
        LOG_INF("Published (buffer: %d pending)",
                offline_buffer_count());
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
    LOG_INF("=== Conexio Offline Buffer Sample ===");
    LOG_INF("Buffer: %d entries | Replay batch: %d | TTL: %ds",
            CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER_SIZE,
            CONFIG_CONEXIO_CLOUD_OFFLINE_REPLAY_BATCH,
            CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER_TTL_SEC);

    conexio_cloud_register_sensor("temperature", read_temperature, NULL);

#if defined(CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER_FULL_CB)
    conexio_cloud_register_offline_buffer_full_cb(on_buffer_full);
#endif

    int ret = conexio_cloud_init(on_cloud_event);
    if (ret) { LOG_ERR("init failed (%d)", ret); return -1; }

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
