/*
 * log_stream.c — Zephyr log backend that forwards log messages to the cloud
 *
 * Copyright (c) 2026 Conexio Technologies, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * ┌────────────────────────────────────────────────────────────────────────┐
 * │  How it works                                                          │
 * │                                                                        │
 * │  Registers itself as a native Zephyr log backend via                  │
 * │  LOG_BACKEND_DEFINE(). Every LOG_INF/WRN/ERR call in the firmware     │
 * │  is captured, formatted, and added to a small ring buffer.            │
 * │                                                                        │
 * │  The ring buffer is drained by the SDK background thread on each      │
 * │  telemetry publish — log entries ride inside the regular JSON payload  │
 * │  as a "_log" array:                                                    │
 * │                                                                        │
 * │    "_log": [                                                           │
 * │      {"l":"WRN","m":"cell_location","s":"meas failed"},               │
 * │      {"l":"ERR","m":"fota","s":"download timeout"}                    │
 * │    ]                                                                   │
 * │                                                                        │
 * │  Key abbreviations keep payload overhead minimal:                      │
 * │    l = level  (DBG/INF/WRN/ERR)                                       │
 * │    m = module (log module name, truncated to 16 chars)                │
 * │    s = string (log message, truncated to CONFIG_CONEXIO_LOG_MSG_LEN)  │
 * │                                                                        │
 * │  When the ring buffer is full the oldest entry is silently dropped.   │
 * │  No flash writes — entirely RAM-based.                                 │
 * │                                                                        │
 * │  Kconfig:                                                              │
 * │    CONFIG_CONEXIO_CLOUD_LOG_STREAM=y     — enable                     │
 * │    CONFIG_CONEXIO_CLOUD_LOG_LEVEL        — min severity (0=DBG…3=ERR) │
 * │    CONFIG_CONEXIO_CLOUD_LOG_QUEUE_SIZE   — ring buffer depth          │
 * │    CONFIG_CONEXIO_LOG_MSG_LEN            — max chars per message      │
 * └────────────────────────────────────────────────────────────────────────┘
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/logging/log_msg.h>
#include <string.h>
#include <stdio.h>
#include "log_stream.h"

/* ── Ring buffer ──────────────────────────────────────────────────────────
 *
 * Fixed-size RAM ring. When full, oldest entry is overwritten (drop-oldest).
 * Protected by a spinlock so the log backend (called from any context,
 * including ISR) and the drain function (called from the SDK thread) are safe.
 */
#define QUEUE_SIZE  CONFIG_CONEXIO_CLOUD_LOG_QUEUE_SIZE
#define MSG_LEN     CONFIG_CONEXIO_LOG_MSG_LEN
#define MODULE_LEN  16

struct log_entry {
    uint8_t level;               /* LOG_LEVEL_DBG/INF/WRN/ERR */
    char    module[MODULE_LEN];  /* log module name             */
    char    msg[MSG_LEN];        /* formatted message string    */
};

static struct log_entry g_ring[QUEUE_SIZE];
static uint16_t g_head  = 0;   /* next write position (oldest-drop on full) */
static uint16_t g_tail  = 0;   /* next read position                         */
static uint16_t g_count = 0;   /* number of valid entries                    */
static struct k_spinlock g_lock;

static inline void ring_push(uint8_t level, const char *module, const char *msg)
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
        /* Full — drop oldest: advance tail */
        g_tail = (g_tail + 1) % QUEUE_SIZE;
    }

    k_spin_unlock(&g_lock, key);
}

/* ── Level → short string ─────────────────────────────────────────────── */
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

/* ── Zephyr log backend callbacks ─────────────────────────────────────── */

/* Output buffer used by log_output to format the message string. */
static uint8_t   g_out_buf[MSG_LEN];
static uint16_t  g_out_pos;

static int log_output_func(uint8_t *data, size_t length, void *ctx)
{
    ARG_UNUSED(ctx);
    size_t copy = MIN(length, (size_t)(MSG_LEN - 1 - g_out_pos));
    memcpy(&g_out_buf[g_out_pos], data, copy);
    g_out_pos += (uint16_t)copy;
    return (int)length;
}

LOG_OUTPUT_DEFINE(g_log_output, log_output_func, g_out_buf, sizeof(g_out_buf));

static void backend_process(const struct log_backend *backend,
                             union log_msg_generic *msg)
{
    ARG_UNUSED(backend);

    uint8_t level = log_msg_get_level(&msg->log);

    /* Respect the configured minimum severity */
    if (level < CONFIG_CONEXIO_CLOUD_LOG_LEVEL) {
        return;
    }

    /* Get module (source) name */
    const char *src = log_msg_get_source_name(&msg->log);

    /* Format the message text into g_out_buf via log_output */
    g_out_pos = 0;
    memset(g_out_buf, 0, sizeof(g_out_buf));

    uint32_t flags = LOG_OUTPUT_FLAG_CRLF_NONE | LOG_OUTPUT_FLAG_FORMAT_SYSLOG;
    log_output_msg_process(&g_log_output, &msg->log, flags);
    g_out_buf[g_out_pos] = '\0';

    /* Strip trailing newlines that log_output adds */
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
    /* In panic mode, disable ourselves so the panic handler can use
     * the serial backend without interference. */
    log_backend_disable(backend);
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

LOG_BACKEND_DEFINE(conexio_log_backend, g_backend_api, true);

/* ── Public API ────────────────────────────────────────────────────────── */

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
