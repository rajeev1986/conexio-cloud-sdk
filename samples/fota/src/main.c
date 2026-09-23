/*
 * Copyright (c) 2026 Conexio Technologies, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

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
 *
 * To build with MCUboot (required for FOTA):
 *   west build -b conexio_stratus_pro/nrf9151/ns --pristine
 *   (sysbuild.conf selects MCUboot automatically)
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

/* ── Maintenance window callback (optional) ──────────────────────────────
 * Return false to defer the download until the device is in a safe state.
 * The SDK retries every CONFIG_FOTA_PAUSE_RETRY_SEC seconds (default: 60s).
 *
 * Uncomment the registration in main() to activate this check.
 */
#if defined(CONFIG_CONEXIO_CLOUD_FOTA)
static bool fota_safe_to_start(void)
{
    /* Always safe in this sample — replace with real checks */
    return true;
}
#endif

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
