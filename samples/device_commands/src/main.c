/*
 * Copyright (c) 2026 Conexio Technologies, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * device_commands/src/main.c — Cloud device commands sample.
 *
 * Demonstrates handling commands sent from the Conexio Console to the device.
 *
 * Built-in SDK commands (registered automatically, no code needed):
 *   REBOOT          — immediately reboots the device
 *   SET_INTERVAL    — changes the telemetry publish interval at runtime
 *   FIRMWARE_UPDATE — triggers an OTA firmware download (requires FOTA=y)
 *
 * Application commands (registered in this file):
 *   LED_ON   — turns on the on-board LED  (gpio0 pin 25)
 *   LED_OFF  — turns off the on-board LED
 *   BUZZ     — custom example command (extend for your hardware)
 *
 * How to test from Conexio Console:
 *   1. Go to your device → Commands tab
 *   2. Select a command from the dropdown (or enter a custom name)
 *   3. Optionally add a JSON payload (e.g. {"duration": 5})
 *   4. Click Send — the device receives it within one MQTT poll cycle (~500ms)
 *
 * Command delivery is QoS 1 — the broker retries until the device ACKs.
 * The SDK sends the dashboard ACK before dispatching to the handler so
 * a REBOOT command won't cause an infinite reboot loop.
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

/* ── Command handlers ─────────────────────────────────────────────────────
 * Each handler receives:
 *   payload_json — raw JSON string from the command payload field (may be "{}").
 *                  Parse with cJSON if you need values from the payload.
 *   arg          — user data pointer passed during registration (NULL here).
 *
 * The SDK has already sent the PUBACK and dashboard ACK before calling these,
 * so it is safe to call sys_reboot() or do any blocking operation here.
 */

static void on_led_on(const char *payload_json, void *arg)
{
    ARG_UNUSED(payload_json); ARG_UNUSED(arg);
    gpio_pin_set_dt(&g_led, 1);
    LOG_INF("Command: LED_ON — LED is ON");
}

static void on_led_off(const char *payload_json, void *arg)
{
    ARG_UNUSED(payload_json); ARG_UNUSED(arg);
    gpio_pin_set_dt(&g_led, 0);
    LOG_INF("Command: LED_OFF — LED is OFF");
}

static void on_buzz(const char *payload_json, void *arg)
{
    ARG_UNUSED(payload_json); ARG_UNUSED(arg);
    /* Extend: parse duration from payload_json, drive a buzzer GPIO, etc. */
    LOG_INF("Command: BUZZ — payload: %s", payload_json ? payload_json : "{}");
}

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
    LOG_INF("=== Conexio Device Commands Sample ===");

    /* LED GPIO init */
    if (gpio_is_ready_dt(&g_led)) {
        gpio_pin_configure_dt(&g_led, GPIO_OUTPUT_INACTIVE);
        LOG_INF("LED ready (pin %d)", g_led.pin);
    }

    /*
     * Register application commands.
     * SDK built-ins (REBOOT, SET_INTERVAL, FIRMWARE_UPDATE) are registered
     * automatically inside conexio_cloud_init() — no entry needed here.
     */
    conexio_cloud_register_command("LED_ON",  on_led_on,  NULL);
    conexio_cloud_register_command("LED_OFF", on_led_off, NULL);
    conexio_cloud_register_command("BUZZ",    on_buzz,    NULL);

    int ret = conexio_cloud_init(on_cloud_event);
    if (ret) { LOG_ERR("init failed (%d)", ret); return -1; }

    LOG_INF("Connecting... Commands will be dispatched on arrival.");
    if (conexio_cloud_wait_connected(60000) == 0) {
        k_sleep(K_SECONDS(2));
        conexio_cloud_publish();
    }

    while (1) {
        k_sleep(K_SECONDS(5));
    }
    return 0;
}
