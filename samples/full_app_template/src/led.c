/*
 * Copyright (c) 2026 Conexio Technologies, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "led.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(led, LOG_LEVEL_INF);

/*
 * The Conexio Stratus Pro has one user-controllable LED:
 *   led0 -> gpio0 pin 25, GPIO_ACTIVE_HIGH
 * Defined in the board DTS (conexio_stratus_pro_common.dtsi).
 * Accessible via the led0 alias -- no overlay entry needed.
 */
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec g_led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

int led_init(void)
{
	if (!gpio_is_ready_dt(&g_led)) {
		LOG_ERR("LED GPIO device not ready");
		return -ENODEV;
	}

	int ret = gpio_pin_configure_dt(&g_led, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		LOG_ERR("LED GPIO configure failed (%d)", ret);
		return ret;
	}

	LOG_INF("LED GPIO ready (pin %d)", g_led.pin);

	/* Boot indicator -- blink LED on for 2 s then off so the user
	 * can confirm the board has started without a serial cable. */
	gpio_pin_set_dt(&g_led, 1);
	k_sleep(K_SECONDS(2));
	gpio_pin_set_dt(&g_led, 0);

	return 0;
}

void led_on(int led_num)
{
	ARG_UNUSED(led_num); /* Only LED 0 is supported currently */
	int ret = gpio_pin_set_dt(&g_led, 1);
	if (ret != 0) {
		LOG_ERR("led_on: gpio_pin_set_dt failed (%d)", ret);
	} else {
		LOG_INF("LED is ON");
	}
}

void led_off(int led_num)
{
	ARG_UNUSED(led_num); /* Only LED 0 is supported currently */
	int ret = gpio_pin_set_dt(&g_led, 0);
	if (ret != 0) {
		LOG_ERR("led_off: gpio_pin_set_dt failed (%d)", ret);
	} else {
		LOG_INF("LED is OFF");
	}
}
