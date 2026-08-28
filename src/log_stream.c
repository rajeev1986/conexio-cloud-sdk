/*
 * log_stream.c — Zephyr log backend that forwards log messages to the cloud
 *
 * Copyright (c) 2026 Conexio Technologies, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Registers a native Zephyr log backend. Every LOG_INF/WRN/ERR call in the
 * firmware is captured and stored in a RAM ring buffer. The SDK background
 * thread drains the buffer onto each telemetry publish as a "_log" JSON array:
 *
 *   "_log": [
 *     {"l":"WRN","m":"cell_location","s":"meas failed"},
 *     {"l":"ERR","m":"fota","s":"download timeout"}
 *   ]
 *
 * Key abbreviations keep payload overhead minimal:
 *   l = level  (DBG/INF/WRN/ERR)
 *   m = module (log module name, max 16 chars)
 *   s = string (formatted message, max CONFIG_CONEXIO_LOG_MSG_LEN chars)
 *
 * When the ring buffer is full the oldest entry is silently dropped.
 * Entirely RAM-based — no flash writes.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_backend_std.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/logging/log_msg.h>
#include <string.h>
#include <cJSON.h>
#include "log_stream.h"

/* Avoid this backend capturing its own log calls (infinite recursion) */
LOG_MODULE_REGISTER(conexio_log_stream, LOG_LEVEL_NONE);

/* ── Ring buffer ──────────────────────────────────────────────────────────── */

#define QUEUE_SIZE  CONFIG_CONEXIO_CLOUD_LOG_QUEUE_SIZE
#define MSG_LEN     CONFIG_CONEXIO_LOG_MSG_LEN
#define MODULE_LEN  16

struct log_entry {
	uint8_t level;
	char    module[MODULE_LEN];
	char    msg[MSG_LEN];
};

static struct log_entry g_ring[QUEUE_SIZE];
static uint16_t g_head  = 0;
static uint16_t g_tail  = 0;
static uint16_t g_count = 0;
static K_SPINLOCK_DEFINE(g_lock);
static bool g_panic_mode = false;

static void ring_push(uint8_t level, const char *module, const char *msg)
{
	k_spinlock_key_t key = k_spin_lock(&g_lock);

	struct log_entry *e = &g_ring[g_head];
	e->level = level;
	strncpy(e->module, module ? module : "?", MODULE_LEN - 1);
	e->module[MODULE_LEN - 1] = '\0';
	strncpy(e->msg, msg ? msg : "", MSG_LEN - 1);
	e->msg[MSG_LEN - 1] = '\0';

	g_head = (g_head + 1) % QUEUE_SIZE;

	if (g_count < QUEUE_SIZE) {
		g_count++;
	} else {
		/* Full — drop oldest */
		g_tail = (g_tail + 1) % QUEUE_SIZE;
	}

	k_spin_unlock(&g_lock, key);
}

/* ── log_output format buffer and callback ────────────────────────────────── */

static uint8_t  g_out_buf[MSG_LEN];
static uint16_t g_out_pos;

static int output_func(uint8_t *data, size_t length, void *ctx)
{
	ARG_UNUSED(ctx);
	size_t copy = MIN(length, (size_t)(MSG_LEN - 1 - g_out_pos));

	memcpy(&g_out_buf[g_out_pos], data, copy);
	g_out_pos += (uint16_t)copy;
	return (int)length;
}

LOG_OUTPUT_DEFINE(g_log_output, output_func, g_out_buf, sizeof(g_out_buf));

/* ── Level → short string ─────────────────────────────────────────────────── */
static const char *level_str(uint8_t level)
{
	switch (level) {
	case LOG_LEVEL_DBG: return "DBG";
	case LOG_LEVEL_INF: return "INF";
	case LOG_LEVEL_WRN: return "WRN";
	case LOG_LEVEL_ERR: return "ERR";
	default:            return "???";
	}
}

/* ── Zephyr log backend callbacks ─────────────────────────────────────────── */

static void backend_process(const struct log_backend *backend,
			    union log_msg_generic *msg)
{
	if (g_panic_mode) {
		return;
	}

	uint8_t level = log_msg_get_level(&msg->log);

	/* Respect the configured minimum severity */
	if (level < (uint8_t)CONFIG_CONEXIO_CLOUD_LOG_LEVEL) {
		return;
	}

	/* Get module name via source_id → log_source_name_get().
	 * This is the correct NCS v3.2.1 API (log_msg_get_source_name
	 * does not exist in this Zephyr version). */
	const char *src = NULL;
	int16_t source_id = log_msg_get_source_id(&msg->log);

	if (source_id >= 0) {
		uint8_t domain_id = log_msg_get_domain(&msg->log);

		src = log_source_name_get(domain_id, (uint32_t)source_id);
	}

	/* Format the message text using the standard Zephyr log output path.
	 * Use LOG_OUTPUT_FLAG_CRLF_NONE to suppress line endings.
	 * Use log_backend_std_get_flags() for format compatibility — this is
	 * the same approach used by log_backend_mqtt.c in NCS v3.2.1. */
	g_out_pos = 0;
	memset(g_out_buf, 0, sizeof(g_out_buf));

	uint32_t flags = LOG_OUTPUT_FLAG_CRLF_NONE;
	log_output_ctx_set(&g_log_output, NULL);
	log_output_msg_process(&g_log_output, &msg->log, flags);
	g_out_buf[g_out_pos] = '\0';

	/* Strip any trailing newline/carriage-return characters */
	for (int i = (int)g_out_pos - 1; i >= 0; i--) {
		if (g_out_buf[i] == '\n' || g_out_buf[i] == '\r') {
			g_out_buf[i] = '\0';
		} else {
			break;
		}
	}

	ring_push(level, src, (const char *)g_out_buf);
}

static void backend_panic(struct log_backend const *backend)
{
	ARG_UNUSED(backend);
	/* Disable in panic mode — let the serial backend take over */
	g_panic_mode = true;
}

static void backend_init(const struct log_backend *backend)
{
	ARG_UNUSED(backend);
}

static const struct log_backend_api g_backend_api = {
	.process = backend_process,
	.panic   = backend_panic,
	.init    = backend_init,
};

/* Register with auto_start=true so the backend is active from boot */
LOG_BACKEND_DEFINE(conexio_log_backend, g_backend_api, true);

/* ── Public API ───────────────────────────────────────────────────────────── */

int log_stream_drain(cJSON *metrics_obj)
{
	if (!metrics_obj) {
		return 0;
	}

	k_spinlock_key_t key = k_spin_lock(&g_lock);
	uint16_t count = g_count;

	k_spin_unlock(&g_lock, key);

	if (count == 0) {
		return 0;
	}

	cJSON *arr = cJSON_CreateArray();

	if (!arr) {
		return -ENOMEM;
	}

	int drained = 0;

	while (true) {
		key = k_spin_lock(&g_lock);
		if (g_count == 0) {
			k_spin_unlock(&g_lock, key);
			break;
		}
		struct log_entry snap = g_ring[g_tail];

		g_tail  = (g_tail + 1) % QUEUE_SIZE;
		g_count--;
		k_spin_unlock(&g_lock, key);

		cJSON *entry = cJSON_CreateObject();

		if (!entry) {
			break;
		}
		cJSON_AddStringToObject(entry, "l", level_str(snap.level));
		cJSON_AddStringToObject(entry, "m", snap.module);
		cJSON_AddStringToObject(entry, "s", snap.msg);
		cJSON_AddItemToArray(arr, entry);
		drained++;
	}

	if (drained > 0) {
		cJSON_AddItemToObject(metrics_obj, "_log", arr);
	} else {
		cJSON_Delete(arr);
	}

	return drained;
}

int log_stream_pending(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_lock);
	int c = (int)g_count;

	k_spin_unlock(&g_lock, key);
	return c;
}

void log_stream_clear(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_lock);

	g_head  = 0;
	g_tail  = 0;
	g_count = 0;
	k_spin_unlock(&g_lock, key);
}
