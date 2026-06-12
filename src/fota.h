#ifndef FOTA_H
#define FOTA_H

#include <stdbool.h>

/**
 * @file fota.h
 * @brief Firmware Over-the-Air update via AWS IoT Jobs + MCUboot
 *
 * Subscribes to the AWS IoT Jobs MQTT topic for this device.
 * When the Conexio Console dashboard triggers a firmware update job,
 * this module downloads the binary via HTTPS and hands it to MCUboot
 * for installation on next reboot.
 *
 * Flow:
 *   1. AWS IoT Job created (from Conexio Console Firmware page)
 *   2. Device receives job notification on $aws/things/<id>/jobs/notify
 *   3. fota_check_and_execute() downloads the firmware binary
 *   4. MCUboot validates the image and marks it for swap
 *   5. Device reboots into new firmware
 *   6. If new firmware calls fota_confirm() within boot_timeout, install
 *      is confirmed. Otherwise MCUboot reverts to previous version.
 */

/** FOTA event types delivered to the application callback. */
enum fota_event_type {
    FOTA_EVT_STARTED,       /**< Download started */
    FOTA_EVT_PROGRESS,      /**< Download in progress — check data.progress_pct */
    FOTA_EVT_COMPLETE,      /**< Download complete — device will reboot */
    FOTA_EVT_FAILED,        /**< Download or validation failed */
    FOTA_EVT_CONFIRMED,     /**< New firmware confirmed (post-reboot) */
};

struct fota_event {
    enum fota_event_type type;
    union {
        int  progress_pct;  /**< 0-100 (FOTA_EVT_PROGRESS) */
        int  error;         /**< errno value (FOTA_EVT_FAILED) */
    } data;
};

typedef void (*fota_event_cb_t)(const struct fota_event *evt);

/**
 * @brief Initialise FOTA module.
 * Subscribes to the IoT Jobs topic for this device.
 * @param device_id  The device's Thing name (IMEI-derived).
 * @param cb         Application callback for FOTA events.
 * @return 0 on success.
 */
int fota_init(const char *device_id, fota_event_cb_t cb);

/**
 * @brief Check for pending IoT Jobs and execute if found.
 * Non-blocking — call periodically or from CONNACK handler.
 * Returns immediately if no job is pending.
 * @return 0 if no job or job successful, negative errno on error.
 */
int fota_check_and_execute(void);

/**
 * @brief Confirm new firmware is working (call once after successful boot).
 * Must be called within MCUboot's confirmation timeout window.
 * If not called, MCUboot reverts to previous firmware on next reboot.
 */
void fota_confirm(void);

/**
 * @brief Returns true if a FOTA update is currently in progress.
 */
bool fota_is_active(void);

#endif /* FOTA_H */
