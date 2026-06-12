/*
 * offline_buffer.c — Flash-backed ring buffer for offline telemetry
 *
 * Uses Zephyr NVS (Non-Volatile Storage) to persist payloads in flash.
 * Survives device reboots — buffered data is not lost on power cycle.
 *
 * NVS key layout (chosen to avoid collision with other SDK NVS users):
 *
 *   Key 0x0010 = write_idx  (uint16_t: next write position)
 *   Key 0x0011 = read_idx   (uint16_t: next read position)
 *   Key 0x0012 = count      (uint16_t: number of valid entries)
 *   Key 0x2000 + i = entry[i] (payload string, OFFLINE_BUFFER_ENTRY_MAX bytes)
 *
 *   SDK NVS key reservations (do not overlap):
 *     0x0001        — reboot counter (conexio_cloud.c)
 *     0x0010-0x0012 — offline buffer metadata (this file)
 *     0x2000+       — offline buffer entries (this file)
 *
 * The buffer is a ring: write_idx wraps at CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER_SIZE.
 * When full, oldest entry is evicted on push.
 *
 * Flash sector size is determined at runtime via flash_get_page_info_by_offs()
 * so the buffer works correctly across all nRF91xx boards regardless of their
 * internal flash page geometry.
 */

#include <zephyr/kernel.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/drivers/flash.h>   /* flash_get_page_info_by_offs */
#include <zephyr/logging/log.h>
#include <string.h>
#include "offline_buffer.h"

LOG_MODULE_REGISTER(offline_buf, LOG_LEVEL_INF);

/* NVS occupies 4 flash sectors in the storage_partition.
 * Sector size is read from the flash driver at init — not hardcoded. */
#define NVS_SECTOR_COUNT 4

/* NVS metadata keys — offset from 0x0001 (reboot counter) to avoid collision */
#define KEY_WRITE_IDX  0x0010
#define KEY_READ_IDX   0x0011
#define KEY_COUNT      0x0012

/* NVS entry keys — 0x2000 base gives ample room for future SDK keys */
#define KEY_ENTRY_BASE 0x2000

#define BUFFER_SIZE CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER_SIZE

static struct nvs_fs fs;
static K_MUTEX_DEFINE(buf_mutex);
static bool initialised = false;

/* In-memory state (mirrors NVS for speed — avoids a read on every operation) */
static uint16_t g_write_idx = 0;
static uint16_t g_read_idx  = 0;
static uint16_t g_count     = 0;

/* ── NVS helpers ──────────────────────────────────────────────────────────── */

static int nvs_read_u16(uint16_t key, uint16_t *val)
{
    ssize_t ret = nvs_read(&fs, key, val, sizeof(*val));
    return (ret < 0) ? (int)ret : 0;
}

static int nvs_write_u16(uint16_t key, uint16_t val)
{
    ssize_t ret = nvs_write(&fs, key, &val, sizeof(val));
    return (ret < 0) ? (int)ret : 0;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int offline_buffer_init(void)
{
    const struct device *flash_dev = FIXED_PARTITION_DEVICE(storage_partition);

    if (!device_is_ready(flash_dev)) {
        LOG_ERR("Offline buffer: flash device not ready");
        return -ENODEV;
    }

    /* Read the actual flash page size at runtime — do not hardcode.
     * Different nRF91xx boards (DK, Thingy:91, custom) can have different
     * internal flash geometry.  This ensures NVS is configured correctly. */
    struct flash_pages_info page_info;
    int ret = flash_get_page_info_by_offs(flash_dev,
                                          FIXED_PARTITION_OFFSET(storage_partition),
                                          &page_info);
    if (ret) {
        LOG_ERR("Offline buffer: cannot read flash page info (%d)", ret);
        return ret;
    }

    fs.flash_device = flash_dev;
    fs.offset       = FIXED_PARTITION_OFFSET(storage_partition);
    fs.sector_size  = page_info.size;   /* runtime, not hardcoded */
    fs.sector_count = NVS_SECTOR_COUNT;

    ret = nvs_mount(&fs);
    if (ret) {
        LOG_ERR("NVS mount failed (%d) — offline buffer unavailable", ret);
        return ret;
    }

    /* Restore state from flash */
    nvs_read_u16(KEY_WRITE_IDX, &g_write_idx);
    nvs_read_u16(KEY_READ_IDX,  &g_read_idx);
    nvs_read_u16(KEY_COUNT,     &g_count);

    /* Sanity check */
    if (g_write_idx >= BUFFER_SIZE) g_write_idx = 0;
    if (g_read_idx  >= BUFFER_SIZE) g_read_idx  = 0;
    if (g_count     >  BUFFER_SIZE) g_count     = 0;

    initialised = true;
    LOG_INF("Offline buffer ready: %d/%d entries",
            g_count, BUFFER_SIZE);
    return 0;
}

int offline_buffer_push(const char *payload, size_t len)
{
    if (!initialised || !payload || len == 0) return -EINVAL;
    if (len >= OFFLINE_BUFFER_ENTRY_MAX) {
        LOG_WRN("Payload too large (%zu) — truncating to %d", len, OFFLINE_BUFFER_ENTRY_MAX - 1);
        len = OFFLINE_BUFFER_ENTRY_MAX - 1;
    }

    k_mutex_lock(&buf_mutex, K_FOREVER);

    /* If full, advance read_idx to drop the oldest entry */
    if (g_count == BUFFER_SIZE) {
        g_read_idx = (g_read_idx + 1) % BUFFER_SIZE;
        nvs_write_u16(KEY_READ_IDX, g_read_idx);
        LOG_DBG("Buffer full — oldest entry dropped");
    } else {
        g_count++;
        nvs_write_u16(KEY_COUNT, g_count);
    }

    /* Write payload at write_idx */
    uint16_t entry_key = (uint16_t)(KEY_ENTRY_BASE + g_write_idx);
    ssize_t written = nvs_write(&fs, entry_key, payload, len);
    if (written < 0) {
        LOG_ERR("NVS write failed (key 0x%04x, err %zd)", entry_key, written);
        k_mutex_unlock(&buf_mutex);
        return (int)written;
    }

    /* Advance write pointer */
    g_write_idx = (g_write_idx + 1) % BUFFER_SIZE;
    nvs_write_u16(KEY_WRITE_IDX, g_write_idx);

    LOG_DBG("Buffered payload (%zu bytes) — %d entries pending", len, g_count);

    k_mutex_unlock(&buf_mutex);
    return 0;
}

int offline_buffer_peek(char *out, size_t *out_len)
{
    if (!initialised || !out || !out_len) return -EINVAL;

    k_mutex_lock(&buf_mutex, K_FOREVER);

    if (g_count == 0) {
        k_mutex_unlock(&buf_mutex);
        return -ENOENT;
    }

    uint16_t entry_key = (uint16_t)(KEY_ENTRY_BASE + g_read_idx);
    ssize_t read_len = nvs_read(&fs, entry_key, out, OFFLINE_BUFFER_ENTRY_MAX - 1);

    k_mutex_unlock(&buf_mutex);

    if (read_len < 0) {
        LOG_ERR("NVS read failed (key 0x%04x, err %zd)", entry_key, read_len);
        return (int)read_len;
    }

    out[read_len] = '\0';
    *out_len = (size_t)read_len;
    return 0;
}

int offline_buffer_pop(void)
{
    if (!initialised) return -EINVAL;

    k_mutex_lock(&buf_mutex, K_FOREVER);

    if (g_count == 0) {
        k_mutex_unlock(&buf_mutex);
        return -ENOENT;
    }

    g_read_idx = (g_read_idx + 1) % BUFFER_SIZE;
    g_count--;

    /* Flush metadata to NVS immediately on pop so a crash between pops
     * doesn't replay already-sent payloads on the next boot. */
    nvs_write_u16(KEY_READ_IDX, g_read_idx);
    nvs_write_u16(KEY_COUNT,    g_count);

    k_mutex_unlock(&buf_mutex);
    return 0;
}

bool offline_buffer_is_empty(void)
{
    return !initialised || g_count == 0;
}

int offline_buffer_count(void)
{
    return (int)g_count;
}

void offline_buffer_clear(void)
{
    if (!initialised) return;
    k_mutex_lock(&buf_mutex, K_FOREVER);
    g_write_idx = 0;
    g_read_idx  = 0;
    g_count     = 0;
    nvs_write_u16(KEY_WRITE_IDX, 0);
    nvs_write_u16(KEY_READ_IDX,  0);
    nvs_write_u16(KEY_COUNT,     0);
    k_mutex_unlock(&buf_mutex);
    LOG_INF("Offline buffer cleared");
}