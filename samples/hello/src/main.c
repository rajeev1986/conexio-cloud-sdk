/*
 * hello/src/main.c — Minimal Conexio Cloud SDK sample.
 *
 * Demonstrates the absolute minimum needed to connect a Conexio Stratus Pro
 * to the Conexio Cloud platform and send a "hello world" metric.
 *
 * The SDK handles:
 *   - LTE attach, NTP sync, TLS certificate management
 *   - MQTT connect / reconnect with exponential backoff
 *   - CBOR binary encoding (50–70% smaller than JSON on LTE-M)
 *   - All SDK auto-metrics (_rssi, _snr, _reboot_cnt, _sdk_version, etc.)
 *
 * The application provides:
 *   - One metric: "uptime_sec" — seconds since boot
 *   - One event handler: logs connected / disconnected
 */

#include <conexio_cloud/conexio_cloud.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if __has_include(<app_version.h>)
#  include <app_version.h>
#else
#  define APP_VERSION_STRING "1.0.0"
#endif

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

/* ── Sensor callback ──────────────────────────────────────────────────────
 * The SDK calls this before every publish. Return a double value to include
 * it in the telemetry payload, or NAN to skip for this cycle.
 */
static double read_uptime(void *arg)
{
    ARG_UNUSED(arg);
    return (double)(k_uptime_get() / 1000);   /* seconds since boot */
}

/* ── Cloud event handler ────────────────────────────────────────────────── */
static void on_cloud_event(const struct conexio_cloud_event *evt)
{
    switch (evt->type) {
    case CONEXIO_CLOUD_EVT_CONNECTED:
        LOG_INF("Connected to Conexio Cloud — device: %s | SDK v%s",
                conexio_cloud_device_id(), CONEXIO_CLOUD_VERSION);
        break;
    case CONEXIO_CLOUD_EVT_DISCONNECTED:
        LOG_WRN("Disconnected");
        break;
    case CONEXIO_CLOUD_EVT_PUBLISHED:
        LOG_INF("Telemetry published");
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
    LOG_INF("=== Conexio Hello Sample ===");
    LOG_INF("App v%s | SDK v%s", APP_VERSION_STRING, CONEXIO_CLOUD_VERSION);

    /* Register the uptime sensor — SDK calls this before each publish */
    conexio_cloud_register_sensor("uptime_sec", read_uptime, NULL);

    /* Single call initialises everything: LTE → NTP → TLS → MQTT thread */
    int ret = conexio_cloud_init(on_cloud_event);
    if (ret) {
        LOG_ERR("conexio_cloud_init failed (%d)", ret);
        return -1;
    }

    /* Wait for first MQTT connection then do a boot publish */
    LOG_INF("Connecting...");
    if (conexio_cloud_wait_connected(60000) == 0) {
        k_sleep(K_SECONDS(2));   /* let RSRP measurement settle */
        conexio_cloud_publish();
    } else {
        LOG_WRN("Connect timeout — will retry in background");
    }

    /* Main loop — the SDK background thread handles everything else */
    while (1) {
        k_sleep(K_SECONDS(5));
    }

    return 0;
}
