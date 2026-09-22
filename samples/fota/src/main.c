/*
 * fota/src/main.c — Firmware Over-the-Air (FOTA) update sample.
 *
 * Demonstrates setting up and handling OTA firmware updates delivered via
 * the Conexio Console Firmware page.
 *
 * How FOTA works:
 *   1. You upload a new firmware binary in Conexio Console → Firmware
 *   2. Console creates a job targeting this device (or all devices)
 *   3. SDK receives the FIRMWARE_UPDATE command with a signed download URL
 *   4. SDK downloads the binary using the nRF9151 modem's HTTPS client
 *   5. MCUboot applies the update on the next reboot
 *   6. _app_fw_version in the next telemetry confirms the new version
 *
 * This sample demonstrates:
 *   - Enabling FOTA (CONFIG_CONEXIO_CLOUD_FOTA=y)
 *   - Optional maintenance window callback — defer updates until safe
 *   - FOTA event callback for progress logging
 *
 * To build with MCUboot (required for FOTA):
 *   west build -b conexio_stratus_pro/nrf9151/ns
 *   (MCUboot is auto-selected when CONFIG_CONEXIO_CLOUD_FOTA=y)
 *
 * See FOTA_BUILD_REFERENCE.md in samples/app/ for signing instructions.
 *
 * Notes:
 *   - Conexio firmware URLs are long (~1700 chars). CONFIG_DOWNLOADER_MAX_FILENAME_SIZE=2048
 *     is set in prj.conf to prevent URL truncation (HTTP 403 on truncated URLs).
 *   - The SDK confirms the new image (prevents MCUboot rollback) automatically.
 *   - _app_fw_version is read from the VERSION file at build time.
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

/* ── FOTA event callback (optional) ──────────────────────────────────────
 * The SDK provides progress events during a firmware download.
 * Register via conexio_cloud_set_fota_cb() for custom progress tracking,
 * status LED updates, or logging.
 */
#if defined(CONFIG_CONEXIO_CLOUD_FOTA)
static void on_fota_event(const struct conexio_fota_event *evt)
{
    switch (evt->type) {
    case CONEXIO_FOTA_EVT_START:
        LOG_INF("FOTA: download started — version: %s",
                evt->data.start.version ? evt->data.start.version : "unknown");
        break;
    case CONEXIO_FOTA_EVT_PROGRESS:
        LOG_INF("FOTA: %d%%", evt->data.progress.progress_percent);
        break;
    case CONEXIO_FOTA_EVT_COMPLETE:
        LOG_INF("FOTA: download complete — rebooting to apply");
        break;
    case CONEXIO_FOTA_EVT_ERROR:
        LOG_ERR("FOTA: error — will retry on next connection");
        break;
    default:
        break;
    }
}

/* ── Maintenance window callback (optional) ──────────────────────────────
 * Return false to defer the download until the device is in a safe state.
 * The SDK retries every CONFIG_FOTA_PAUSE_RETRY_SEC seconds.
 *
 * Example use cases:
 *   - Motor / actuator is running — unsafe to reboot mid-cycle
 *   - Measurement in progress — don't interrupt data collection
 *   - Battery below threshold — avoid reboot on low battery
 *
 * Uncomment the registration in main() to activate this check.
 */
static bool fota_safe_to_start(void)
{
    /* Always safe in this sample — replace with real checks */
    return true;
}
#endif /* CONFIG_CONEXIO_CLOUD_FOTA */

/* ── Cloud event handler ────────────────────────────────────────────────── */
static void on_cloud_event(const struct conexio_cloud_event *evt)
{
    switch (evt->type) {
    case CONEXIO_CLOUD_EVT_CONNECTED:
        LOG_INF("Connected — %s | App v%s | SDK v%s",
                conexio_cloud_device_id(), APP_VERSION_STRING,
                CONEXIO_CLOUD_VERSION);
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
    LOG_INF("=== Conexio FOTA Sample ===");
    LOG_INF("Current firmware: v%s", APP_VERSION_STRING);

#if defined(CONFIG_CONEXIO_CLOUD_FOTA)
    /* Optional: register FOTA event callback for progress logging */
    conexio_cloud_set_fota_cb(on_fota_event);

    /* Optional: register maintenance window check
     * Uncomment to defer downloads until fota_safe_to_start() returns true */
    /* conexio_cloud_set_fota_can_start_cb(fota_safe_to_start); */
    ARG_UNUSED(fota_safe_to_start);
#endif

    int ret = conexio_cloud_init(on_cloud_event);
    if (ret) { LOG_ERR("init failed (%d)", ret); return -1; }

    LOG_INF("Connecting — trigger a FOTA job from Conexio Console → Firmware.");
    if (conexio_cloud_wait_connected(60000) == 0) {
        k_sleep(K_SECONDS(2));
        conexio_cloud_publish();
    }

    while (1) {
        k_sleep(K_SECONDS(5));
    }
    return 0;
}
