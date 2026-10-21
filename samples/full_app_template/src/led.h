/*
 * Copyright (c) 2026 Conexio Technologies, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file led.h
 * @brief Platform-agnostic LED control API for Conexio samples.
 *
 * The implementation (led.c) uses the Zephyr GPIO DT API and may be
 * replaced per platform without changing this header or its callers.
 */

#ifndef CONEXIO_LED_H
#define CONEXIO_LED_H

/**
 * @brief Initialize the LED subsystem.
 *
 * Configures the on-board LED as an output (initially OFF), then
 * performs a 2-second boot-indicator blink so the user can confirm
 * the board has started without a serial cable.
 *
 * Must be called before led_on() or led_off().
 *
 * @return 0 on success, negative errno on failure.
 */
int led_init(void);

/**
 * @brief Turn on the specified LED.
 *
 * @param led_num LED index (0-based). Only LED 0 is currently supported.
 */
void led_on(int led_num);

/**
 * @brief Turn off the specified LED.
 *
 * @param led_num LED index (0-based). Only LED 0 is currently supported.
 */
void led_off(int led_num);

#endif /* CONEXIO_LED_H */
