#ifndef OFFLINE_BUFFER_H
#define OFFLINE_BUFFER_H

#include <stdbool.h>
#include <stddef.h>

/**
 * @file offline_buffer.h
 * @brief Flash-backed ring buffer for offline telemetry payloads.
 *
 * When the device loses cloud connectivity, telemetry payloads are stored
 * in flash using Zephyr NVS. On reconnection, buffered payloads are replayed
 * to the cloud in chronological order before resuming live telemetry.
 *
 * Buffer capacity: CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER_SIZE payloads.
 * Overflow policy: oldest entry is overwritten (ring buffer).
 *
 * NVS key allocation (do not overlap with other NVS users):
 *   0x0010–0x0012  — buffer metadata (write_idx, read_idx, count)
 *   0x2000+        — payload entries
 *   0x0001         — reboot counter (reserved by conexio_cloud.c)
 *
 * Usage:
 *   1. Call offline_buffer_init() once at startup.
 *   2. When offline: offline_buffer_push(payload, len)
 *   3. On reconnect: while (!offline_buffer_is_empty()) {
 *                        offline_buffer_peek(buf, &len);
 *                        send(buf, len);
 *                        offline_buffer_pop();
 *                    }
 */

/** Maximum size of a single buffered payload (bytes). */
#define OFFLINE_BUFFER_ENTRY_MAX 512

/**
 * @brief Initialise the offline buffer (mounts NVS partition).
 * @return 0 on success.
 */
int offline_buffer_init(void);

/**
 * @brief Push a payload into the offline buffer.
 * If the buffer is full, the oldest entry is overwritten.
 * @param payload  JSON string to buffer.
 * @param len      Length of payload in bytes.
 * @return 0 on success, negative errno on flash error.
 */
int offline_buffer_push(const char *payload, size_t len);

/**
 * @brief Read the oldest buffered payload without removing it.
 * @param out      Buffer to write payload into (must be OFFLINE_BUFFER_ENTRY_MAX).
 * @param out_len  Populated with the actual payload length.
 * @return 0 on success, -ENOENT if buffer is empty.
 */
int offline_buffer_peek(char *out, size_t *out_len);

/**
 * @brief Remove the oldest buffered payload (call after successful send).
 * @return 0 on success.
 */
int offline_buffer_pop(void);

/**
 * @brief Returns true if the buffer contains no entries.
 */
bool offline_buffer_is_empty(void);

/**
 * @brief Returns the number of payloads currently buffered.
 */
int offline_buffer_count(void);

/**
 * @brief Erase all buffered payloads.
 */
void offline_buffer_clear(void);

#endif /* OFFLINE_BUFFER_H */
