/*
 * log_stream.h — Cloud log streaming backend (internal SDK header)
 *
 * Copyright (c) 2026 Conexio Technologies, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Registers a Zephyr log backend that captures LOG_INF/WRN/ERR calls and
 * stores them in a RAM ring buffer. The SDK background thread drains the
 * buffer onto each telemetry payload as a "_log" JSON array.
 *
 * Applications do not call these functions directly. The SDK core
 * (conexio_cloud.c) calls log_stream_drain() from build_payload() when
 * CONFIG_CONEXIO_CLOUD_LOG_STREAM=y.
 */

#ifndef LOG_STREAM_H_
#define LOG_STREAM_H_

#include <cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Drain all pending log entries into a "_log" JSON array on
 *        the given metrics object.
 *
 * Called by build_payload() in conexio_cloud.c. Adds the "_log" key only
 * if there is at least one pending entry.
 *
 * @param metrics_obj  The cJSON "metrics" object to add "_log" to.
 * @return Number of entries drained, or negative errno on error.
 */
int log_stream_drain(cJSON *metrics_obj);

/**
 * @brief Returns the number of log entries currently pending in the ring buffer.
 */
int log_stream_pending(void);

/**
 * @brief Discard all pending log entries (e.g. before a FOTA reboot).
 */
void log_stream_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* LOG_STREAM_H_ */
