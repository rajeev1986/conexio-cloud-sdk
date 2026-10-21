/*
 * Copyright (c) 2026 Conexio Technologies, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "led.h"
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(led, LOG_LEVEL_INF);

/*
 * All six supported boards (Conexio Stratus Pro, nRF9151-DK, nRF9160-DK,
 * nRF9161-DK, Thingy:91, Thingy:91 X) define a led0 alias in their base DTSI,
 * so the DT_HAS_ALIAS(led0) active branch is always taken for those boards.
 * The #else no-op branch exists for forward compatibility with any future
 * board that has no user-accessible LED.
 */
#if DT_HAS_ALIAS(led0)

static const struct gpio_dt_spec g_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

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
	ret = gpio_pin_set_dt(&g_led, 1);
	if (ret != 0) {
		LOG_ERR("LED boot-blink on failed (%d)", ret);
	}
	k_sleep(K_SECONDS(2));
	ret = gpio_pin_set_dt(&g_led, 0);
	if (ret != 0) {
		LOG_ERR("LED boot-blink off failed (%d)", ret);
	}

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

#else /* !DT_HAS_ALIAS(led0) */

int led_init(void)
{
	LOG_DBG("No led0 alias defined for this board -- LED init is a no-op");
	return 0;
}

void led_on(int led_num)
{
	ARG_UNUSED(led_num);
	LOG_DBG("No led0 alias defined for this board -- LED on is a no-op");
}

void led_off(int led_num)
{
	ARG_UNUSED(led_num);
	LOG_DBG("No led0 alias defined for this board -- LED off is a no-op");
}

#endif /* DT_HAS_ALIAS(led0) */
