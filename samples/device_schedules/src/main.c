/*
 * Copyright (c) 2026 Conexio Technologies, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * device_schedules/src/main.c — Device Schedules sample.
 *
 * Device Schedules allow the Conexio Console to send two timed commands to
 * the device: a START command at a specified time and a STOP command at
 * another time. The SDK stores the schedule in NVS flash — if the device
 * loses power or connectivity between the two, the STOP command is still
 * executed autonomously when the firmware boots or reconnects.
 *
 * This sample demonstrates:
 *   - Registering LED_ON / LED_OFF as schedule start/stop commands
 *   - Receiving the schedule lifecycle callback (STARTED, STOPPED, EXPIRED)
 *   - The firmware watchdog timer that fires STOP even without connectivity
 *
 * How to test from Conexio Console:
 *   1. Go to your device → Schedules tab
 *   2. Create a schedule:
 *        Start command:  LED_ON
 *        Stop command:   LED_OFF
 *        Start time:     <pick a time ~2 minutes from now>
 *        Stop time:      <pick a time ~5 minutes from now>
 *   3. At the start time: LED turns on, log shows "Schedule watchdog armed"
 *   4. At the stop time:  LED turns off, log shows "Schedule stopped"
 *   5. Reboot during the active window → LED_OFF fires immediately on boot
 *
 * The NVS persistence ensures the schedule survives power loss — the SDK
 * checks the stored schedule on every boot and fires the stop command if
 * the stop time has already passed.
 */

#include <conexio_cloud/conexio_cloud.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#if __has_include(<app_version.h>)
#  include <app_version.h>
#else
#  define APP_VERSION_STRING "1.0.0"
#endif

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec g_led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/* ── Command handlers ─────────────────────────────────────────────────── */

static void on_led_on(const char *payload_json, void *arg)
{
    ARG_UNUSED(payload_json); ARG_UNUSED(arg);
    gpio_pin_set_dt(&g_led, 1);
    LOG_INF("LED_ON — LED is ON");
}

static void on_led_off(const char *payload_json, void *arg)
{
    ARG_UNUSED(payload_json); ARG_UNUSED(arg);
    gpio_pin_set_dt(&g_led, 0);
    LOG_INF("LED_OFF — LED is OFF");
    /* Cancel the firmware watchdog — STOP has arrived, timer no longer needed */
    conexio_cloud_cancel_schedule_watchdog();
}

/* ── Schedule lifecycle callback ──────────────────────────────────────────
 * The SDK calls this at three points in the schedule lifecycle:
 *
 *   STARTED  — start command received from cloud; firmware watchdog armed.
 *              The SDK stores stopCommand + stopAt in NVS.
 *
 *   STOPPED  — stop time passed while offline; firmware timer fired.
 *              LED_OFF was executed autonomously without cloud connectivity.
 *
 *   EXPIRED  — device rebooted while inside the schedule window.
 *              LED_OFF was executed immediately on boot.
 */
static void on_schedule(const struct conexio_schedule_event *evt)
{
    switch (evt->type) {
    case CONEXIO_SCHEDULE_EVT_STARTED:
        LOG_INF("Schedule STARTED — '%s' will fire at stopAt (watchdog armed)",
                evt->stop_command);
        break;
    case CONEXIO_SCHEDULE_EVT_STOPPED:
        LOG_INF("Schedule STOPPED — '%s' fired autonomously (firmware timer)",
                evt->stop_command);
        break;
    case CONEXIO_SCHEDULE_EVT_EXPIRED:
        LOG_WRN("Schedule EXPIRED on boot — '%s' executed immediately",
                evt->stop_command);
        break;
    default:
        break;
    }
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
    LOG_INF("=== Conexio Device Schedules Sample ===");

    /* LED GPIO init */
    if (gpio_is_ready_dt(&g_led)) {
        gpio_pin_configure_dt(&g_led, GPIO_OUTPUT_INACTIVE);
        LOG_INF("LED ready (pin %d)", g_led.pin);
    }

    /* Register the commands used by the schedule start/stop */
    conexio_cloud_register_command("LED_ON",  on_led_on,  NULL);
    conexio_cloud_register_command("LED_OFF", on_led_off, NULL);

    /* Register the schedule lifecycle callback
     * The SDK stores the schedule in NVS — this callback gets notified on
     * start, autonomous stop, and boot-time expiry. */
    conexio_cloud_register_schedule_cb(on_schedule);

    int ret = conexio_cloud_init(on_cloud_event);
    if (ret) { LOG_ERR("init failed (%d)", ret); return -1; }

    LOG_INF("Ready — create a schedule in Conexio Console to test.");
    if (conexio_cloud_wait_connected(60000) == 0) {
        k_sleep(K_SECONDS(2));
        conexio_cloud_publish();
    }

    while (1) {
        k_sleep(K_SECONDS(5));
    }
    return 0;
}
