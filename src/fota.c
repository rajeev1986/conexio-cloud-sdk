/*
 * fota.c — Firmware Over-the-Air update via AWS IoT Jobs + MCUboot
 *
 * Integrates with the Conexio Console dashboard Firmware page:
 *   1. Admin uploads a firmware binary via the dashboard
 *   2. Admin creates a job targeting this device (or all devices)
 *   3. AWS IoT Jobs notifies the device on its jobs/notify topic
 *   4. This module downloads the binary, validates it, prepares MCUboot
 *   5. Device reboots — MCUboot swaps images
 *   6. New firmware calls fota_confirm() to prevent rollback
 */

#include <zephyr/kernel.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/dfu/flash_img.h>
#include <net/fota_download.h>
#include <zephyr/net/http/parser_url.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <cJSON.h>
#include <cJSON_os.h>
#include <stdio.h>
#include <string.h>
#include "fota.h"
#include "transport.h"     /* transport_publish_raw() */

LOG_MODULE_REGISTER(fota, LOG_LEVEL_INF);

/*
 * AWS IoT Jobs MQTT topics for this device.
 * The SDK's AWS IoT integration subscribes to these automatically
 * when CONFIG_AWS_IOT_JOBS_ENABLED=y.
 */
#define JOBS_NOTIFY_TOPIC_FMT   "$aws/things/%s/jobs/notify"
#define JOBS_UPDATE_TOPIC_FMT   "$aws/things/%s/jobs/%s/update"
#define JOB_STATUS_IN_PROGRESS  "{\"status\":\"IN_PROGRESS\",\"statusDetails\":{\"step\":\"downloading\"}}"
#define JOB_STATUS_SUCCEEDED    "{\"status\":\"SUCCEEDED\"}"
#define JOB_STATUS_FAILED_FMT   "{\"status\":\"FAILED\",\"statusDetails\":{\"reason\":\"%s\"}}"

static fota_event_cb_t  g_cb            = NULL;
static bool             g_fota_active   = false;
static char             g_device_id[32] = {0};
static char             g_current_job_id[64] = {0};

/* ── NVS: persist pending SUCCEEDED publish across reboot ─────────────────── */
/*
 * When FOTA completes and the device reboots, MQTT is not yet connected so
 * job_status_publish("SUCCEEDED") fails. We persist the completed job ID to
 * NVS before the reboot so fota_check_and_execute() can publish SUCCEEDED
 * on the first MQTT connect of the new firmware.
 *
 * NVS key 0x0005 is reserved for this purpose.
 * Layout in NVS record: "<device_id>\n<job_id>\0"  (max 32+1+64+1 = 98 bytes)
 *
 * NVS key allocation (from conexio_cloud.c comment):
 *   0x0001 reboot counter
 *   0x0002 reboot reason
 *   0x0003 schedule watchdog active flag
 *   0x0004 schedule watchdog record
 *   0x0005 FOTA pending SUCCEEDED  ← this file
 *   0x0010–0x0012, 0x2000+ offline buffer
 */
#define FOTA_PENDING_NVS_ID  0x0005U
#define FOTA_PENDING_MAX     98U    /* device_id(31) + '\n'(1) + job_id(63) + '\0'(1) + margin */

#define NVS_PARTITION        storage_partition
#define NVS_PARTITION_DEVICE FIXED_PARTITION_DEVICE(NVS_PARTITION)
#define NVS_PARTITION_OFFSET FIXED_PARTITION_OFFSET(NVS_PARTITION)

static struct nvs_fs  g_fota_nvs;
static bool           g_fota_nvs_ready = false;

static int fota_nvs_init(void)
{
    if (g_fota_nvs_ready) return 0;

    struct flash_pages_info info;
    g_fota_nvs.flash_device = NVS_PARTITION_DEVICE;
    if (!device_is_ready(g_fota_nvs.flash_device)) return -ENODEV;
    g_fota_nvs.offset = NVS_PARTITION_OFFSET;
    int rc = flash_get_page_info_by_offs(g_fota_nvs.flash_device,
                                         g_fota_nvs.offset, &info);
    if (rc) return rc;
    g_fota_nvs.sector_size  = info.size;
    g_fota_nvs.sector_count = 2U;
    rc = nvs_mount(&g_fota_nvs);
    if (rc) return rc;
    g_fota_nvs_ready = true;
    return 0;
}

/* Persist "<device_id>\n<job_id>" to NVS so SUCCEEDED can be sent after reboot */
static void fota_pending_save(const char *device_id, const char *job_id)
{
    if (fota_nvs_init() != 0) return;
    char buf[FOTA_PENDING_MAX];
    int len = snprintf(buf, sizeof(buf), "%s\n%s", device_id, job_id);
    if (len < 0 || len >= (int)sizeof(buf)) return;
    nvs_write(&g_fota_nvs, FOTA_PENDING_NVS_ID, buf, (uint16_t)(len + 1));
    LOG_INF("FOTA: pending SUCCEEDED saved for job %s", job_id);
}

/* Load and clear pending job from NVS. Returns true if a pending job was found. */
static bool fota_pending_load(char *device_id_out, size_t dev_size,
                               char *job_id_out,    size_t job_size)
{
    if (fota_nvs_init() != 0) return false;
    char buf[FOTA_PENDING_MAX];
    ssize_t rc = nvs_read(&g_fota_nvs, FOTA_PENDING_NVS_ID, buf, sizeof(buf));
    if (rc <= 0) return false;
    buf[sizeof(buf) - 1] = '\0';

    /* Split on '\n' */
    char *sep = strchr(buf, '\n');
    if (!sep) return false;
    *sep = '\0';
    strncpy(device_id_out, buf,    dev_size - 1);
    strncpy(job_id_out,    sep+1,  job_size - 1);
    device_id_out[dev_size - 1] = '\0';
    job_id_out[job_size - 1]    = '\0';
    return (device_id_out[0] != '\0' && job_id_out[0] != '\0');
}

/* Erase the pending record after successful publish */
static void fota_pending_clear(void)
{
    if (!g_fota_nvs_ready) return;
    nvs_delete(&g_fota_nvs, FOTA_PENDING_NVS_ID);
    LOG_DBG("FOTA: pending SUCCEEDED cleared");
}

/* ── AWS IoT Jobs status reporting ────────────────────────────────────────── */
/*
 * Publish a status update to the AWS IoT Jobs service so the backend
 * EventBridge rule can update DynamoDB and the frontend in real time.
 *
 * Uses snprintf-only construction (no cJSON/heap) so it is safe to call
 * from the downloader thread which has a small stack (CONFIG_DOWNLOADER_STACK_SIZE).
 *
 * Topic: $aws/things/{deviceId}/jobs/{jobId}/update
 * Payload examples:
 *   {"status":"IN_PROGRESS","statusDetails":{"step":"downloading","progress":"42"}}
 *   {"status":"IN_PROGRESS","statusDetails":{"step":"installing"}}
 *   {"status":"SUCCEEDED"}
 *   {"status":"FAILED","statusDetails":{"reason":"download_error"}}
 */
static void job_status_publish(const char *status, const char *step,
                               const char *reason, int progress_pct)
{
    if (!g_device_id[0] || !g_current_job_id[0]) return;

    /* Build topic — stack allocated, bounded */
    char topic[128];
    int topic_len = snprintf(topic, sizeof(topic),
                             "$aws/things/%s/jobs/%s/update",
                             g_device_id, g_current_job_id);
    if (topic_len < 0 || topic_len >= (int)sizeof(topic)) return;

    /* Build payload with snprintf — no heap allocation, safe on small stacks */
    char payload[192];
    int payload_len;

    if (step && progress_pct >= 0) {
        payload_len = snprintf(payload, sizeof(payload),
            "{\"status\":\"%s\",\"statusDetails\":{\"step\":\"%s\",\"progress\":\"%d\"}}",
            status, step, progress_pct);
    } else if (step) {
        payload_len = snprintf(payload, sizeof(payload),
            "{\"status\":\"%s\",\"statusDetails\":{\"step\":\"%s\"}}",
            status, step);
    } else if (reason) {
        payload_len = snprintf(payload, sizeof(payload),
            "{\"status\":\"%s\",\"statusDetails\":{\"reason\":\"%s\"}}",
            status, reason);
    } else {
        payload_len = snprintf(payload, sizeof(payload),
            "{\"status\":\"%s\"}",
            status);
    }
    if (payload_len < 0 || payload_len >= (int)sizeof(payload)) return;

    int ret = transport_publish_raw(topic, payload, (size_t)payload_len);
    if (ret == 0) {
        LOG_DBG("IoT Job status: %s (step=%s)", status, step ? step : "-");
    } else {
        LOG_WRN("IoT Job status publish failed (%d) — UI may not update", ret);
    }
}



static void fota_download_handler(const struct fota_download_evt *evt)
{
    if (!g_cb) return;

    struct fota_event app_evt = {0};

    switch (evt->id) {

    case FOTA_DOWNLOAD_EVT_PROGRESS:
        app_evt.type              = FOTA_EVT_PROGRESS;
        app_evt.data.progress_pct = evt->progress;
        g_cb(&app_evt);
        /* Progress bar: overwrite same line using \r (no newline).
         * 20-char bar: filled with '#', empty with '.'.
         * Only print when percentage changes to reduce serial noise. */
        {
            static int last_pct = -1;
            int pct = evt->progress;
            if (pct != last_pct) {
                last_pct = pct;
                int filled = pct / 5;   /* 0-20 blocks */
                /* \r returns cursor to line start; no \n so it overwrites */
                printk("\r\033[KFOTA [");
                for (int i = 0; i < 20; i++) {
                    printk(i < filled ? "#" : ".");
                }
                printk("] %3d%%", pct);
                if (pct >= 100) {
                    printk("\n");   /* final newline at 100% */
                    last_pct = -1; /* reset for next FOTA */
                }
            }
        }
        /* Do NOT publish IoT Job status on progress events — the modem
         * radio is occupied with the HTTPS download and any MQTT publish
         * attempt will compete for the socket, causing broker disconnects.
         * Status is reported at start (IN_PROGRESS), completion (installing),
         * and post-reboot confirmation (SUCCEEDED) only. */
        break;

    case FOTA_DOWNLOAD_EVT_FINISHED:
        LOG_INF("FOTA download complete — requesting reboot");
        /* Report 'installing' — binary is written to flash, MCUboot swap pending */
        job_status_publish("IN_PROGRESS", "installing", NULL, -1);
        app_evt.type = FOTA_EVT_COMPLETE;
        g_cb(&app_evt);

        /* Give the app 2 seconds to clean up, then reboot into new firmware */
        k_sleep(K_SECONDS(2));
        sys_reboot(SYS_REBOOT_WARM);
        break;

    case FOTA_DOWNLOAD_EVT_ERASE_PENDING:
        LOG_INF("FOTA: erasing flash...");
        break;

    case FOTA_DOWNLOAD_EVT_ERASE_DONE:
        LOG_INF("FOTA: flash erase complete");
        break;

    case FOTA_DOWNLOAD_EVT_ERROR:
        LOG_ERR("FOTA download failed");
        g_fota_active = false;
        job_status_publish("FAILED", NULL, "download_error", -1);
        app_evt.type       = FOTA_EVT_FAILED;
        app_evt.data.error = -EIO;
        g_cb(&app_evt);
        break;

    case FOTA_DOWNLOAD_EVT_ERASE_TIMEOUT:
        /* NCS v3.2.1: fired when flash erase takes too long.
         * Treat as a fatal error — the download cannot continue. */
        LOG_ERR("FOTA: flash erase timed out — aborting");
        g_fota_active = false;
        job_status_publish("FAILED", NULL, "erase_timeout", -1);
        app_evt.type       = FOTA_EVT_FAILED;
        app_evt.data.error = -ETIMEDOUT;
        g_cb(&app_evt);
        break;

    case FOTA_DOWNLOAD_EVT_CANCELLED:
        /* Download was cancelled (e.g. by the cloud or a reboot request).
         * Reset state so a new job can start. */
        LOG_WRN("FOTA: download cancelled");
        g_fota_active = false;
        break;

    default:
        break;
    }
}

/* ── Parse job document and start download ────────────────────────────────── */

static int execute_job(const char *job_id, const char *job_document)
{
    /*
     * Conexio Console firmware job document format (matches firmware/handler.ts):
     * {
     *   "operation":       "firmware_update",
     *   "firmwareVersion": "1.4.2",
     *   "location": {
     *     "url": "https://iot-dashboard-firmware-123.s3.amazonaws.com/fw-v1.4.2.bin"
     *   },
     *   "checksum":  "sha256:abc123...",
     *   "fileSize":  131072
     * }
     *
     * The download URL is a 24-hour pre-signed S3 GET URL generated by the
     * Firmware Lambda — the device does not need AWS credentials to fetch it.
     */
    cJSON *doc = cJSON_Parse(job_document);
    if (!doc) {
        LOG_ERR("Job document parse failed");
        return -EINVAL;
    }

    /* Extract URL from location.url */
    const cJSON *location = cJSON_GetObjectItem(doc, "location");
    if (!location) {
        LOG_ERR("Job document missing 'location' key");
        cJSON_Delete(doc);
        return -EINVAL;
    }

    const char *url     = cJSON_GetStringValue(cJSON_GetObjectItem(location, "url"));
    const char *version = cJSON_GetStringValue(cJSON_GetObjectItem(doc, "firmwareVersion"));

    if (!url) {
        LOG_ERR("Job document missing location.url");
        cJSON_Delete(doc);
        return -EINVAL;
    }

    LOG_INF("FOTA job: version=%s", version ? version : "unknown");
    LOG_INF("Download URL: %s", url);

    strncpy(g_current_job_id, job_id, sizeof(g_current_job_id) - 1);
    g_fota_active = true;

    struct fota_event start_evt = { .type = FOTA_EVT_STARTED };
    if (g_cb) g_cb(&start_evt);

    /* Notify AWS IoT Jobs that download is starting */
    job_status_publish("IN_PROGRESS", "downloading", NULL, 0);

    /* Split the presigned URL into host and file (path + query string).
     *
     * fota_download_start() requires host and file as separate arguments.
     * The downloader joins them as snprintf("%s/%s", host, file), so file
     * must NOT have a leading '/'.
     *
     * We use http_parser_parse_url() — the same approach as Nordic's own
     * aws_fota_json.c — instead of manual strchr() for robustness:
     *   - Handles URLs with explicit port numbers (https://host:443/path)
     *   - Returns zero-length fields for missing components, no bad pointers
     *   - Correctly splits path + query into the file string
     *
     * Static buffers: pointers passed to fota_download_start() must remain
     * valid until the download completes (NCS requirement).
     */
    static char g_host_buf[CONFIG_DOWNLOADER_MAX_HOSTNAME_SIZE];
    static char g_file_buf[CONFIG_DOWNLOADER_MAX_FILENAME_SIZE];

    struct http_parser_url u;
    http_parser_url_init(&u);

    if (http_parser_parse_url(url, strlen(url), false, &u) != 0) {
        LOG_ERR("Failed to parse firmware URL");
        g_fota_active = false;
        cJSON_Delete(doc);
        return -EINVAL;
    }

    uint16_t schema_off = u.field_data[UF_SCHEMA].off;
    uint16_t schema_len = u.field_data[UF_SCHEMA].len;
    uint16_t host_off   = u.field_data[UF_HOST].off;
    uint16_t host_len   = u.field_data[UF_HOST].len;

    /* Build "https://hostname" (include port if present) */
    int host_written;
    if (u.field_set & (1 << UF_PORT)) {
        host_written = snprintf(g_host_buf, sizeof(g_host_buf),
                                "%.*s://%.*s:%u",
                                schema_len, url + schema_off,
                                host_len,   url + host_off,
                                u.port);
    } else {
        host_written = snprintf(g_host_buf, sizeof(g_host_buf),
                                "%.*s://%.*s",
                                schema_len, url + schema_off,
                                host_len,   url + host_off);
    }
    if (host_written < 0 || host_written >= (int)sizeof(g_host_buf)) {
        LOG_ERR("FOTA host buffer too small");
        g_fota_active = false;
        cJSON_Delete(doc);
        return -ENOMEM;
    }

    /* Extract file: path + query, strip the leading '/' so the downloader's
     * snprintf("%s/%s", host, file) produces one slash, not "//".
     * Mirrors aws_fota_json.c: url + UF_PATH.off + 1
     */
    uint16_t path_off  = u.field_data[UF_PATH].off;
    uint16_t path_len  = u.field_data[UF_PATH].len;
    uint16_t query_off = u.field_data[UF_QUERY].off;
    uint16_t query_len = u.field_data[UF_QUERY].len;

    if (path_len == 0) {
        LOG_ERR("FOTA URL has no path component");
        g_fota_active = false;
        cJSON_Delete(doc);
        return -EINVAL;
    }

    int file_written;
    if (query_len > 0) {
        /* path (without leading '/') + '?' + query */
        file_written = snprintf(g_file_buf, sizeof(g_file_buf), "%.*s?%.*s",
                                path_len - 1, url + path_off + 1,
                                query_len,    url + query_off);
    } else {
        file_written = snprintf(g_file_buf, sizeof(g_file_buf), "%.*s",
                                path_len - 1, url + path_off + 1);
    }
    if (file_written < 0 || file_written >= (int)sizeof(g_file_buf)) {
        LOG_ERR("FOTA file buffer too small — increase CONFIG_DOWNLOADER_MAX_FILENAME_SIZE");
        g_fota_active = false;
        cJSON_Delete(doc);
        return -ENOMEM;
    }

    LOG_DBG("FOTA host: %s", g_host_buf);
    LOG_DBG("FOTA file: %.80s...", g_file_buf);

    int ret = fota_download_start(g_host_buf, g_file_buf,
                                  CONFIG_CONEXIO_CLOUD_CA_TAG,
                                  0,  /* pdn_id: 0 = default PDN            */
                                  0); /* fragment_size: 0 = modem default    */
    if (ret) {
        LOG_ERR("fota_download_start failed (%d)", ret);
        g_fota_active = false;
        cJSON_Delete(doc);
        return ret;
    }

    cJSON_Delete(doc);
    return 0;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int fota_init(const char *device_id, fota_event_cb_t cb)
{
    if (!device_id) return -EINVAL;

    strncpy(g_device_id, device_id, sizeof(g_device_id) - 1);
    g_cb = cb;

    /* Register fota_download callback */
    int ret = fota_download_init(fota_download_handler);
    if (ret) {
        LOG_ERR("fota_download_init failed (%d)", ret);
        return ret;
    }

    LOG_INF("FOTA module ready for device: %s", g_device_id);
    return 0;
}

int fota_check_and_execute(void)
{
    if (g_fota_active) {
        LOG_DBG("FOTA already in progress");
        return 0;
    }

    /* Post-reboot: check if a SUCCEEDED publish is pending from a completed FOTA.
     * The job_status_publish("SUCCEEDED") in fota_confirm() can't run immediately
     * after reboot because MQTT isn't connected yet. It is persisted to NVS and
     * published here on the first MQTT CONNACK of the new firmware. */
    char pending_dev[32] = {0};
    char pending_job[64] = {0};
    if (fota_pending_load(pending_dev, sizeof(pending_dev),
                          pending_job, sizeof(pending_job))) {
        LOG_INF("FOTA: publishing pending SUCCEEDED for job %s", pending_job);

        char topic[128];
        int topic_len = snprintf(topic, sizeof(topic),
                                 "$aws/things/%s/jobs/%s/update",
                                 pending_dev, pending_job);
        if (topic_len > 0 && topic_len < (int)sizeof(topic)) {
            const char *payload = "{\"status\":\"SUCCEEDED\"}";
            int ret = transport_publish_raw(topic, payload, strlen(payload));
            if (ret == 0) {
                LOG_INF("FOTA: SUCCEEDED published — dashboard will update to Completed");
                fota_pending_clear();
            } else {
                /* Will retry on next CONNACK */
                LOG_WRN("FOTA: SUCCEEDED publish failed (%d) — will retry", ret);
            }
        }
    }

    /*
     * In this SDK implementation FOTA is entirely command-driven.
     * The Conexio Console Firmware page creates an AWS IoT Job and
     * sends it to the device as a FIRMWARE_UPDATE command on the
     * devices/<id>/commands MQTT topic.  The builtin_on_firmware_update()
     * handler in conexio_cloud.c picks it up and calls fota_handle_command().
     */
    return 0;
}

/**
 * Called by main.c command handler when a "FIRMWARE_UPDATE" command
 * arrives with a job document. This is the hook between the SDK
 * command path and the FOTA download.
 */
int fota_handle_command(const char *job_id, const char *job_document)
{
    if (!job_id || !job_document) return -EINVAL;
    return execute_job(job_id, job_document);
}

void fota_confirm(void)
{
    int ret = boot_write_img_confirmed();
    if (ret) {
        LOG_ERR("boot_write_img_confirmed failed (%d) — "
                "device will revert to previous firmware on next reboot", ret);
    } else {
        LOG_INF("Firmware update confirmed — MCUboot will not revert");
        /* Persist job ID to NVS so SUCCEEDED can be published after MQTT reconnects.
         * We cannot publish now — MQTT is not connected immediately after reboot.
         * fota_check_and_execute() will pick this up on the first CONNACK. */
        if (g_device_id[0] && g_current_job_id[0]) {
            fota_pending_save(g_device_id, g_current_job_id);
        }
        struct fota_event evt = { .type = FOTA_EVT_CONFIRMED };
        if (g_cb) g_cb(&evt);
    }
}

bool fota_is_active(void)
{
    return g_fota_active;
}
