/*
 * conexio_cloud.c — Conexio Cloud SDK core (Phase 2)
 *
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │  What this file does                                                    │
 * │                                                                         │
 * │  This is the SDK core.  It sits between the application (main.c) and   │
 * │  the transport layer (mqtt_transport.c / http_transport.c).             │
 * │                                                                         │
 * │  From the application's perspective:                                    │
 * │    1. Register command handlers by name.                                │
 * │    2. Register typed setting handlers by key.                           │
 * │    3. Call conexio_cloud_init() — one call handles everything.          │
 * │    4. Queue metrics with conexio_cloud_send_metric().                   │
 * │       The SDK publishes them automatically on a schedule.               │
 * │                                                                         │
 * │  Phase 2 difference from Phase 1:                                       │
 * │    - conexio_cloud_init() fetches cloud endpoints at runtime using       │
 * │      config_fetch() instead of reading them from Kconfig.               │
 * │    - The Root CA is downloaded from the URL in the fetched config        │
 * │      rather than being embedded in the firmware.                        │
 * │    - Everything else (command dispatch, settings dispatch, telemetry    │
 * │      publishing, reconnection) is identical to Phase 1.                │
 * └─────────────────────────────────────────────────────────────────────────┘
 *
 * Architecture overview
 * ─────────────────────
 *
 *   main.c  ─── registers commands/settings ──▶  cmd_registry[]
 *           ─── calls conexio_cloud_init() ────▶  (boot sequence below)
 *           ─── calls conexio_cloud_send_metric() ▶ metric_queue[]
 *
 *   Boot sequence inside conexio_cloud_init():
 *     1. Increment NVS-backed reboot counter (_reboot_cnt metric).
 *     2. Read IMEI from modem → use as device ID (e.g. "351358815179730").
 *     3. Call config_fetch(imei, &cfg) → get mqtt_host, http_host, api_key,
 *        root_ca_url from the Conexio config service.
 *     4. Call cert_store_provision_from_config(&cfg) → download+store Root CA,
 *        verify device cert+key are in modem.
 *     5. Call transport_init_with_config(device_id, &cfg) → set up transport.
 *     6. Connect to LTE (if CONEXIO_CLOUD_MANAGE_LTE=y).
 *     7. Sync time via NTP (date_time_update_async).
 *     8. Spawn the SDK background thread (cloud_thread_fn).
 *
 *   SDK background thread (cloud_thread_fn):
 *     - Calls transport_connect() when disconnected (with 10 s retry).
 *     - Calls transport_poll() every 500 ms to drive the MQTT event loop.
 *     - Calls conexio_cloud_publish() every CONFIG_CONEXIO_CLOUD_INTERVAL_SEC
 *       to send all queued metrics.
 *
 * ── Automatic metrics (added to every telemetry payload) ─────────────────
 *
 *  Signal quality (refreshed every CONFIG_CONEXIO_CLOUD_MODEM_INFO_REFRESH):
 *   _rssi           RSRP signal strength in dBm
 *   _snr            Signal-to-Noise Ratio index (SNR_IDX_TO_DB(x) = x-24 dB)
 *
 *  Device health (every publish):
 *   _reboot_cnt     Monotonically increasing reboot counter (NVS-persisted)
 *   _sdk_version    SDK semantic version string
 *   _modem_fw       Modem firmware version string (e.g. "mfw_nrf9160_1.3.6")
 *   _operator       Network operator name (e.g. "Telia", "AT&T")
 *   _modem_temp     Modem die temperature in °C (overheating detection)
 *
 *  Radio/network context (every publish, from LTE events):
 *   _lte_mode       Active radio mode: 7=LTE-M, 9=NB-IoT
 *   _lte_band       Active LTE band number (e.g. 3, 20, 28)
 *   _cell_id        E-UTRAN cell ID (decimal) — cell-level location proxy
 *   _tac            Tracking Area Code — area-level location proxy
 *
 *  Connectivity health (accumulated since boot):
 *   _lte_connect_ms Time from boot to first LTE registration (ms)
 *   _conn_loss      Number of LTE drop+re-register events since boot
 *   _reset_loop     1 if modem detected a reset loop this session
 *
 *  Data usage (accumulated since boot, requires MODEM_INFO_CONNECTIVITY):
 *   _tx_kb          Kilobytes transmitted this session
 *   _rx_kb          Kilobytes received this session
 *
 *  PSM/eDRX (only when network has confirmed the parameters):
 *   _psm_tau_sec    Actual granted PSM TAU in seconds (-1 if not granted)
 *   _psm_active_sec Actual granted PSM active window in seconds
 *   _edrx_ms        Actual granted eDRX interval in milliseconds
 *   _edrx_ptw_ms    Actual granted eDRX paging time window in ms
 *
 *  Battery (when CONFIG_CONEXIO_CLOUD_AUTO_BATTERY=y):
 *   _battery_mv     Battery voltage in millivolts
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <cJSON.h>                   /* JSON serialisation for telemetry   */
#include <cJSON_os.h>                /* Zephyr memory adapter for cJSON    */
#include <modem/modem_info.h>         /* IMEI, RSRP, SNR from modem         */
#include <date_time.h>                /* NTP time sync + ISO-8601 timestamp */
#include <math.h>                     /* isnan() for sensor callback return */

#include "conexio_cloud/conexio_cloud.h"  /* Public API (included by app)  */
#include "transport.h"               /* Internal transport interface       */
#include "cert_store.h"              /* TLS credential management          */
#include "lte.h"                     /* LTE connection helper               */
#include "config_fetch.h"            /* Phase 2: runtime config fetch      */

/* ── Optional SDK modules — compiled in based on Kconfig ─────────────── */
#if defined(CONFIG_CONEXIO_CLOUD_RETRY)
#include "retry.h"
#endif
#if defined(CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER)
#include "offline_buffer.h"
#endif
#if defined(CONFIG_CONEXIO_CLOUD_PSM)
#include "power_mgr.h"
#endif
#if defined(CONFIG_CONEXIO_CLOUD_FOTA)
#include "fota.h"
#include <zephyr/dfu/mcuboot.h>   /* boot_is_img_confirmed */
#include <zephyr/sys/reboot.h>
#endif

/* SDK semantic version — reported in every telemetry payload as _sdk_version */
#define CONEXIO_SDK_VERSION "2.1.0"

LOG_MODULE_REGISTER(conexio_cloud, LOG_LEVEL_INF);

/* ── Command registry ─────────────────────────────────────────────────────
 *
 * Application code calls conexio_cloud_register_command("NAME", handler, arg)
 * before conexio_cloud_init().  Entries are stored here.
 *
 * When a command arrives from the cloud (via MQTT or HTTP polling),
 * dispatch_command() looks up the name in this table and calls the handler
 * directly.  No switch statement needed in application code.
 *
 * name   — pointer to the string literal supplied by the caller.
 *           It is NOT copied — the caller must ensure the string lives
 *           as long as the SDK (usually a string literal, so forever).
 * handler — function called with (payload_json, arg) when the command fires.
 * arg    — optional user pointer forwarded to the handler (can be NULL).
 */
struct cmd_entry {
    const char               *name;
    conexio_command_handler_t  handler;
    void                      *arg;
};

static struct cmd_entry cmd_registry[CONFIG_CONEXIO_CLOUD_MAX_COMMANDS];
static int cmd_count = 0;   /* Number of registered commands */

/* ── Settings registry ────────────────────────────────────────────────────
 *
 * Application code calls conexio_cloud_register_setting_int/bool/float/string()
 * before init.  When a config push arrives from the OTA Config page, the SDK
 * iterates over the config JSON object.  For each key it finds a matching
 * entry here, validates the type, and calls the handler with the value
 * already converted to the correct C type.
 *
 * The application never sees raw JSON — it receives a typed value directly.
 */
enum setting_type {
    SETTING_INT,    /* Handler receives int32_t                  */
    SETTING_BOOL,   /* Handler receives bool                     */
    SETTING_FLOAT,  /* Handler receives float                    */
    SETTING_STRING, /* Handler receives const char *, size_t len */
};

struct setting_entry {
    const char       *key;   /* Setting key name (e.g. "telemetryIntervalSec") */
    enum setting_type type;
    union {
        conexio_int_setting_cb_t    cb_int;
        conexio_bool_setting_cb_t   cb_bool;
        conexio_float_setting_cb_t  cb_float;
        conexio_string_setting_cb_t cb_string;
    };
    void *arg;
};

static struct setting_entry setting_registry[CONFIG_CONEXIO_CLOUD_MAX_SETTINGS];
static int setting_count = 0;   /* Number of registered settings */

/* ── Metric queue ─────────────────────────────────────────────────────────
 *
 * Metrics are buffered here between publishes.  The queue is protected by
 * queue_mutex because the application thread writes (send_metric) and the
 * SDK background thread reads (build_payload) concurrently.
 *
 * If the same metric name is queued twice, the second call overwrites the
 * first (update-in-place semantics) — only the latest value is published.
 *
 * type field: 'n' = number (double), 's' = string, 'b' = boolean.
 */
#define MAX_METRIC_NAME 32

struct metric_entry {
    char   name[MAX_METRIC_NAME];
    char   type;          /* 'n', 's', or 'b' */
    double num_val;        /* Used when type == 'n' */
    char   str_val[64];   /* Used when type == 's' */
    bool   bool_val;       /* Used when type == 'b' */
    bool   used;           /* true = this slot contains a pending metric */
};

static struct metric_entry metric_queue[CONFIG_CONEXIO_CLOUD_METRIC_QUEUE_SIZE];
static K_MUTEX_DEFINE(queue_mutex);   /* Guards metric_queue[] */

/* ── Module-level state ───────────────────────────────────────────────────*/

static conexio_cloud_event_cb_t user_cb;          /* Application event callback  */
static char  g_device_id[16];                      /* 15-digit IMEI string      */
static bool  g_initialised = false;                /* Init guard (call only once) */

/* ── Sensor registry ──────────────────────────────────────────────────────
 *
 * Applications call conexio_cloud_register_sensor("name", read_fn, arg)
 * instead of calling conexio_cloud_send_metric() in the main loop.
 * The SDK background thread calls all registered sensor callbacks before
 * each publish, so the application loop reduces to just k_sleep().
 *
 * Callbacks return double (the sensor reading).  Return NaN to skip a metric
 * for a particular cycle (e.g. sensor temporarily unavailable).
 */
#define MAX_SENSOR_NAME 32

struct sensor_entry {
    char   name[MAX_SENSOR_NAME];
    conexio_sensor_read_cb_t callback;
    void  *arg;
    bool   used;
};

static struct sensor_entry sensor_registry[CONFIG_CONEXIO_CLOUD_MAX_SENSORS];
static int sensor_count = 0;

/* ── NTP sync semaphore ───────────────────────────────────────────────────
 *
 * date_time_update_async() is non-blocking.  We register a callback so
 * conexio_cloud_init() can wait for sync rather than using a blind k_sleep().
 * Times out after CONFIG_CONEXIO_CLOUD_NTP_TIMEOUT_SEC seconds.
 */
static K_SEM_DEFINE(ntp_ready_sem, 0, 1);
static bool g_ntp_synced = false;

static void ntp_event_handler(const struct date_time_evt *evt)
{
    if (evt->type == DATE_TIME_OBTAINED_NTP || evt->type == DATE_TIME_OBTAINED_MODEM) {
        LOG_INF("NTP synced");
        g_ntp_synced = true;
        k_sem_give(&ntp_ready_sem);
    }
}

/* ── Reboot counter ───────────────────────────────────────────────────────
 *
 * g_reboot_cnt is incremented on every boot (including power cycles,
 * watchdog resets, and firmware crashes) and included in every telemetry
 * payload as the _reboot_cnt metric.
 *
 * The cloud connectivity/tracker.ts compares each incoming value against the
 * last stored one.  Any increase is recorded as a reboot event with the
 * telemetry timestamp.  These events appear in the Fleet Health →
 * Reboot Tracking tab.
 *
 * NVS persistence:
 *   When CONFIG_NVS=y the counter is stored in the internal flash using
 *   Zephyr's Non-Volatile Storage (NVS) subsystem.  This means the counter
 *   survives power-off and accumulates across the device's entire lifetime.
 *
 *   When CONFIG_NVS is not set, the counter resets to 0 on every boot.
 *   Reboots are still detectable (the counter goes 0 → 1 → 0 → 1 ...) but
 *   the cumulative count is lost.
 *
 * NVS key ID:
 *   REBOOT_CNT_NVS_ID = 1  (must not clash with other NVS users in the app)
 */
#if defined(CONFIG_NVS)
#include <zephyr/fs/nvs.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

/* NVS filesystem lives in the 'storage_partition' flash partition.
 * This partition is defined in the board DTS file. */
#define NVS_PARTITION        storage_partition
#define NVS_PARTITION_DEVICE FIXED_PARTITION_DEVICE(NVS_PARTITION)
#define NVS_PARTITION_OFFSET FIXED_PARTITION_OFFSET(NVS_PARTITION)
/*
 * NVS key IDs — must not clash with each other or with offline_buffer.c.
 *   0x0001 = reboot counter   (this file)
 *   0x0002 = reboot reason    (this file)
 *   0x0010-0x0012 = offline buffer metadata
 *   0x2000+ = offline buffer entries
 */
#define REBOOT_CNT_NVS_ID    0x0001U
#define REBOOT_REASON_NVS_ID 0x0002U

static struct nvs_fs reboot_nvs;
static bool nvs_initialised = false;

/*
 * load_and_increment_reboot_count — read, increment, and write the counter.
 *
 * On the very first boot nvs_read() returns -ENOENT (key not found), leaving
 * count at 0.  We increment to 1 and write it back, so the first boot
 * reports _reboot_cnt = 1.
 *
 * Lazy init: the NVS filesystem is only mounted on the first call.
 * If the flash device isn't ready we return 0 (no persistent count).
 */
static uint32_t load_and_increment_reboot_count(void)
{
    if (!nvs_initialised) {
        struct flash_pages_info info;
        reboot_nvs.flash_device = NVS_PARTITION_DEVICE;
        reboot_nvs.offset       = NVS_PARTITION_OFFSET;

        if (!device_is_ready(reboot_nvs.flash_device)) {
            LOG_WRN("NVS flash device not ready — reboot counter will not persist");
            return 0;
        }

        /* NVS needs to know the flash page size and how many pages to use */
        flash_get_page_info_by_offs(reboot_nvs.flash_device,
                                    reboot_nvs.offset, &info);
        reboot_nvs.sector_size  = info.size;
        reboot_nvs.sector_count = 2U; /* Two pages is the minimum for NVS */

        if (nvs_mount(&reboot_nvs) != 0) {
            LOG_WRN("NVS mount failed — reboot counter will not persist");
            return 0;
        }
        nvs_initialised = true;
    }

    uint32_t count = 0;
    /* -ENOENT on first boot is normal — count stays 0, will be written as 1 */
    nvs_read(&reboot_nvs, REBOOT_CNT_NVS_ID, &count, sizeof(count));
    count++;
    nvs_write(&reboot_nvs, REBOOT_CNT_NVS_ID, &count, sizeof(count));
    return count;
}
#endif /* CONFIG_NVS */

static uint32_t g_reboot_cnt = 0;

/* reboot_counter_init — called once from conexio_cloud_init() */
static void reboot_counter_init(void)
{
#if defined(CONFIG_NVS)
    g_reboot_cnt = load_and_increment_reboot_count();
    LOG_INF("Reboot counter: %u (persisted in NVS)", g_reboot_cnt);
#else
    g_reboot_cnt = 0;
    LOG_DBG("Reboot counter: 0 (NVS disabled — not persistent)");
#endif
}

/* ── Reboot reason ────────────────────────────────────────────────────────
 *
 * Reads the hardware reset cause register via Zephyr hwinfo driver and
 * converts the bitmask to a short human-readable string published as
 * _reboot_reason in every telemetry payload.
 *
 * Supported reasons on nRF9160 (from hwinfo_nrf.c + hwinfo.h):
 *   "watchdog"  RESET_WATCHDOG    — WDT expired; main loop stalled/deadlocked
 *   "lockup"    RESET_CPU_LOCKUP  — CPU lockup / hard fault; firmware crash
 *   "brownout"  RESET_BROWNOUT    — supply voltage collapsed; power issue
 *   "software"  RESET_SOFTWARE    — sys_reboot() called; intentional reset
 *   "pin"       RESET_PIN         — external reset pin; button or supervisor IC
 *   "por"       RESET_POR         — power-on reset; first boot after power off
 *   "wake"      RESET_LOW_POWER_WAKE — woke from System OFF / deep PSM sleep
 *   "debug"     RESET_DEBUG       — debugger/programmer triggered reset
 *   "unknown"   0 or hwinfo error — register empty or driver unavailable
 *
 * Multiple flags can be set simultaneously (register accumulates on nRF9160).
 * We return the single most actionable one, prioritised by severity:
 *   watchdog > lockup > brownout > software > pin > por > wake > debug
 *
 * The register is cleared after reading so the next boot gets a fresh value.
 */
#if defined(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>

#define REBOOT_REASON_LEN 16U
static char g_reboot_reason[REBOOT_REASON_LEN] = "unknown";

static const char *reason_to_string(uint32_t cause)
{
    if (cause & RESET_WATCHDOG)       return "watchdog";
    if (cause & RESET_CPU_LOCKUP)     return "lockup";
    if (cause & RESET_BROWNOUT)       return "brownout";
    if (cause & RESET_SOFTWARE)       return "software";
    if (cause & RESET_PIN)            return "pin";
    if (cause & RESET_POR)            return "por";
    if (cause & RESET_LOW_POWER_WAKE) return "wake";
    if (cause & RESET_DEBUG)          return "debug";
    return "unknown";
}

static void reboot_reason_init(void)
{
    uint32_t cause = 0;
    int ret = hwinfo_get_reset_cause(&cause);
    if (ret != 0) {
        LOG_WRN("hwinfo_get_reset_cause failed (%d)", ret);
        strncpy(g_reboot_reason, "unknown", sizeof(g_reboot_reason) - 1);
        return;
    }

    /* Clear now so the next boot reads a fresh value, not accumulated flags */
    hwinfo_clear_reset_cause();

    strncpy(g_reboot_reason, reason_to_string(cause),
            sizeof(g_reboot_reason) - 1);
    g_reboot_reason[sizeof(g_reboot_reason) - 1] = '\0';

    LOG_INF("Reboot reason: %s (raw=0x%08X)", g_reboot_reason, cause);

#if defined(CONFIG_NVS)
    /* Persist alongside the reboot counter so it's available even if the
     * device reboots again before the first successful cloud publish. */
    if (nvs_initialised) {
        nvs_write(&reboot_nvs, REBOOT_REASON_NVS_ID,
                  g_reboot_reason, sizeof(g_reboot_reason));
    }
#endif
}

#else  /* CONFIG_HWINFO not enabled */
static char g_reboot_reason[16] = "unavailable";
static void reboot_reason_init(void)
{
    LOG_DBG("CONFIG_HWINFO not set — add to prj.conf for reboot reason tracking");
}
#endif /* CONFIG_HWINFO */

/* ── Cloud background thread ─────────────────────────────────────────────
 *
 * Spawned at the end of conexio_cloud_init().
 * Runs at the lowest application priority so it doesn't starve user threads.
 */
static K_THREAD_STACK_DEFINE(cloud_stack, CONFIG_CONEXIO_CLOUD_THREAD_STACK_SIZE);
static struct k_thread cloud_thread_data;

/* ── Application event callback forwarding ───────────────────────────────
 *
 * The SDK intercepts connection events to run its own internal housekeeping
 * (offline buffer replay, FOTA check, retry accounting, PSM sleep) before
 * forwarding to the application callback.  This keeps all SDK logic here and
 * leaves the application callback for app-level reactions only.
 * user_cb is declared in module-level state above and set in conexio_cloud_init().
 */

/* ── Internal cloud event handler ─────────────────────────────────────────
 *
 * Handles all SDK-internal housekeeping on connection lifecycle events.
 * After SDK work is done, forwards the event to the user callback.
 */
static void sdk_internal_event_handler(const struct conexio_cloud_event *evt)
{
    switch (evt->type) {

    case CONEXIO_CLOUD_EVT_CONNECTED:
        LOG_INF("Cloud connected — device: %s", g_device_id);

#if defined(CONFIG_CONEXIO_CLOUD_RETRY)
        /* Connection successful — reset the failure counter so the next
         * disconnect starts backoff from the base interval again. */
        retry_on_success();
#endif

#if defined(CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER)
        /* Replay any payloads that were buffered while the device was offline.
         * Sends up to CONFIG_CONEXIO_CLOUD_OFFLINE_REPLAY_BATCH per session
         * to avoid flooding the server after a long outage. */
        if (!offline_buffer_is_empty()) {
            int pending = offline_buffer_count();
            LOG_INF("Offline buffer: replaying %d payload(s)", pending);
            int replayed = 0;
            while (!offline_buffer_is_empty() &&
                   replayed < CONFIG_CONEXIO_CLOUD_OFFLINE_REPLAY_BATCH) {
                char buf[OFFLINE_BUFFER_ENTRY_MAX];
                size_t buf_len;
                if (offline_buffer_peek(buf, &buf_len) != 0) break;
                /* Publish the buffered raw payload directly via transport
                 * rather than rebuilding — preserves original timestamp. */
                if (transport_publish(buf, buf_len) != 0) {
                    LOG_WRN("Offline replay publish failed — stopping replay");
                    break;
                }
                offline_buffer_pop();
                replayed++;
#if defined(CONFIG_CONEXIO_CLOUD_RETRY)
                retry_kick_watchdog();
#endif
            }
            LOG_INF("Offline replay: sent %d/%d payloads", replayed, pending);
        }
#endif /* CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER */

#if defined(CONFIG_CONEXIO_CLOUD_FOTA)
        /* Check for any pending AWS IoT Jobs immediately on connect */
        fota_check_and_execute();
#endif
        break;

    case CONEXIO_CLOUD_EVT_DISCONNECTED:
        LOG_WRN("Cloud disconnected");
#if defined(CONFIG_CONEXIO_CLOUD_RETRY)
        /* Increment failure counter and apply exponential backoff delay.
         * If max_attempts is reached, retry_on_failure() reboots. */
        retry_on_failure();
#endif
        break;

    case CONEXIO_CLOUD_EVT_PUBLISHED:
        LOG_DBG("Telemetry published");
#if defined(CONFIG_CONEXIO_CLOUD_PSM)
        /* Publishing is done — allow the modem to enter PSM sleep.
         * The modem will wake autonomously at the next TAU interval. */
        power_mgr_sleep();
#endif
        break;

    case CONEXIO_CLOUD_EVT_ERROR:
        LOG_ERR("SDK error: %d", evt->data.error);
#if defined(CONFIG_CONEXIO_CLOUD_RETRY)
        retry_on_failure();
#endif
        break;

    default:
        break;
    }

    /* Forward to the application callback (may be NULL) */
    if (user_cb) {
        user_cb(evt);
    }
}

/* ── Built-in command handlers ────────────────────────────────────────────
 *
 * These commands are registered automatically by the SDK when the
 * corresponding Kconfig option is enabled.  Applications do not need to
 * register them — they just work out of the box.
 *
 * Applications can still register their OWN handler for these names, which
 * will shadow the built-in one (first match wins in dispatch_command).
 *
 * Built-in commands:
 *   REBOOT           — sys_reboot(SYS_REBOOT_COLD) with 500 ms log flush
 *   SET_INTERVAL     — updates the SDK publish interval at runtime
 *   FIRMWARE_UPDATE  — passes job document to fota_handle_command()
 */

/* REBOOT — always registered, no Kconfig guard needed */
static void builtin_on_reboot(const char *payload_json, void *arg)
{
    ARG_UNUSED(payload_json); ARG_UNUSED(arg);
    LOG_INF("REBOOT command — rebooting in 500 ms");
    k_sleep(K_MSEC(500));
    sys_reboot(SYS_REBOOT_COLD);
}

/* SET_INTERVAL — updates the SDK-managed publish interval */
static int g_sdk_interval_sec = CONFIG_CONEXIO_CLOUD_INTERVAL_SEC;

static void builtin_on_set_interval(const char *payload_json, void *arg)
{
    ARG_UNUSED(arg);
    if (!payload_json) return;
    cJSON *p = cJSON_Parse(payload_json);
    if (!p) return;
    const cJSON *iv = cJSON_GetObjectItem(p, "interval");
    if (cJSON_IsNumber(iv)) {
        int new_sec = (int)iv->valuedouble;
        if (new_sec >= 10 && new_sec <= 3600) {
            g_sdk_interval_sec = new_sec;
            LOG_INF("SET_INTERVAL: publish interval → %ds", new_sec);
        } else {
            LOG_WRN("SET_INTERVAL: %d out of range [10, 3600] — ignoring", new_sec);
        }
    }
    cJSON_Delete(p);
}

/* ── Built-in telemetryIntervalSec setting handler ────────────────────────
 * Registered automatically when CONFIG_CONEXIO_CLOUD_AUTO_INTERVAL_SETTING=y.
 * Keeps the SDK publish interval and SET_INTERVAL command in sync with the
 * OTA Config page — application doesn't need to register this key. */
static enum conexio_setting_status builtin_on_interval_setting(int32_t value, void *arg)
{
    ARG_UNUSED(arg);
    if (value < 10 || value > 3600) return CONEXIO_SETTING_VALUE_OUT_OF_RANGE;
    g_sdk_interval_sec = (int)value;
    LOG_INF("SDK: telemetryIntervalSec → %ds", g_sdk_interval_sec);
    return CONEXIO_SETTING_OK;
}

/* Default FOTA event handler — just logs progress.
 * Applications can supply their own via conexio_cloud_set_fota_cb(). */
static fota_event_cb_t g_fota_user_cb = NULL;

static void sdk_fota_event_handler(const struct fota_event *evt)
{
    switch (evt->type) {
    case FOTA_EVT_STARTED:   LOG_INF("FOTA: update started");                          break;
    case FOTA_EVT_PROGRESS:  LOG_INF("FOTA: %d%%", evt->data.progress_pct);            break;
    case FOTA_EVT_COMPLETE:  LOG_INF("FOTA: download complete — rebooting");            break;
    case FOTA_EVT_FAILED:    LOG_ERR("FOTA: failed (err %d)", evt->data.error);         break;
    case FOTA_EVT_CONFIRMED: LOG_INF("FOTA: new firmware confirmed");                  break;
    default: break;
    }
    if (g_fota_user_cb) g_fota_user_cb(evt);
}

/* FIRMWARE_UPDATE command — parses job document and starts download */
static void builtin_on_firmware_update(const char *payload_json, void *arg)
{
    ARG_UNUSED(arg);
    if (!payload_json) return;
    cJSON *p = cJSON_Parse(payload_json);
    if (!p) return;
    const char *job_id = cJSON_GetStringValue(cJSON_GetObjectItem(p, "jobId"));
    const cJSON *doc   = cJSON_GetObjectItem(p, "document");
    if (job_id && doc) {
        char *doc_str = cJSON_PrintUnformatted(doc);
        if (doc_str) {
            fota_handle_command(job_id, doc_str);
            cJSON_FreeString(doc_str);
        }
    }
    cJSON_Delete(p);
}
#endif /* CONFIG_CONEXIO_CLOUD_FOTA */

/* dispatch_error — routes an SDK init error through the internal handler
 * so it reaches both SDK housekeeping and the user callback consistently. */
static void dispatch_error(int err)
{
    struct conexio_cloud_event evt = {
        .type       = CONEXIO_CLOUD_EVT_ERROR,
        .data.error = err,
    };
    sdk_internal_event_handler(&evt);
}

/* ── Command dispatch ─────────────────────────────────────────────────────
 *
 * Called by transport_on_message() when a message with type="command" arrives.
 *
 * Searches cmd_registry[] by name.  Linear search is fine since the registry
 * is small (max CONFIG_CONEXIO_CLOUD_MAX_COMMANDS, default 16).
 *
 * payload_json may be NULL if the cloud sent a command with no payload.
 * Handlers must guard against NULL before parsing.
 */
static void dispatch_command(const char *name, const char *payload_json)
{
    for (int i = 0; i < cmd_count; i++) {
        if (strcmp(cmd_registry[i].name, name) == 0) {
            cmd_registry[i].handler(payload_json, cmd_registry[i].arg);
            return;
        }
    }
    /* Log unhandled commands so developers know they need to register them */
    LOG_WRN("Unhandled command: '%s' — register with "
            "conexio_cloud_register_command()", name);
}

/* ── Settings dispatch ────────────────────────────────────────────────────
 *
 * Called by transport_on_message() for each key in a type="config" message.
 * Validates the JSON type against the registered handler type before calling.
 *
 * If the key is not registered we log at DEBUG and continue — this is
 * intentional: a new config key pushed from the dashboard should not
 * crash a device running older firmware.
 */
static void dispatch_setting(const char *key, const cJSON *value_item)
{
    for (int i = 0; i < setting_count; i++) {
        if (strcmp(setting_registry[i].key, key) != 0) continue;

        enum conexio_setting_status st = CONEXIO_SETTING_OK;
        struct setting_entry *s = &setting_registry[i];

        switch (s->type) {

        case SETTING_INT:
            if (!cJSON_IsNumber(value_item)) {
                LOG_WRN("Setting '%s': expected int, got wrong type", key);
                st = CONEXIO_SETTING_VALUE_WRONG_TYPE;
            } else {
                /* Cast double to int32 — all JSON numbers are doubles */
                st = s->cb_int((int32_t)value_item->valuedouble, s->arg);
            }
            break;

        case SETTING_BOOL:
            if (!cJSON_IsBool(value_item)) {
                LOG_WRN("Setting '%s': expected bool, got wrong type", key);
                st = CONEXIO_SETTING_VALUE_WRONG_TYPE;
            } else {
                st = s->cb_bool(cJSON_IsTrue(value_item), s->arg);
            }
            break;

        case SETTING_FLOAT:
            if (!cJSON_IsNumber(value_item)) {
                LOG_WRN("Setting '%s': expected float, got wrong type", key);
                st = CONEXIO_SETTING_VALUE_WRONG_TYPE;
            } else {
                st = s->cb_float((float)value_item->valuedouble, s->arg);
            }
            break;

        case SETTING_STRING: {
            const char *str = cJSON_GetStringValue((cJSON *)value_item);
            if (!str) {
                LOG_WRN("Setting '%s': expected string, got wrong type", key);
                st = CONEXIO_SETTING_VALUE_WRONG_TYPE;
            } else {
                st = s->cb_string(str, strlen(str), s->arg);
            }
            break;
        }
        }

        /* CONEXIO_SETTING_OK = handler accepted and applied the value */
        if (st != CONEXIO_SETTING_OK) {
            LOG_WRN("Setting '%s' rejected by handler (status %d)", key, (int)st);
        } else {
            LOG_DBG("Setting '%s' applied successfully", key);
        }
        return;
    }

    /* Key not in registry — safe to ignore; could be a newer config key
     * from the dashboard that this firmware version doesn't support yet */
    LOG_DBG("Setting '%s' has no registered handler — ignoring", key);
}

/* ── Inbound message router ───────────────────────────────────────────────
 *
 * Called by the transport layer whenever a message arrives from the cloud.
 * This is the bridge between raw MQTT/HTTP bytes and typed application callbacks.
 *
 * Expected message formats:
 *
 *   Command push:
 *   { "type": "command", "command": "FAN_ON", "commandId": "...",
 *     "sk": "...", "payload": {"speed": 80}, "source": "dashboard" }
 *
 *   Config push (OTA Config page):
 *   { "type": "config", "version": 3, "configId": "cfg-...",
 *     "config": { "telemetryIntervalSec": 120, "debugMode": false } }
 *
 * Unknown types are silently dropped (forward-compatible with new message
 * types added to the cloud in future dashboard versions).
 */
void transport_on_message(const char *json_str, size_t len)
{
    if (!json_str || len == 0) return;

    /* Copy to a heap buffer for cJSON — ensures null termination.
     * k_malloc from the Zephyr system heap (size configured via
     * CONFIG_HEAP_MEM_POOL_SIZE). */
    char *buf = k_malloc(len + 1);
    if (!buf) {
        LOG_WRN("transport_on_message: out of heap memory");
        return;
    }
    memcpy(buf, json_str, len);
    buf[len] = '\0';

    cJSON *msg = cJSON_Parse(buf);
    k_free(buf); /* Free regardless of parse result */
    if (!msg) {
        LOG_WRN("Failed to parse incoming message JSON");
        return;
    }

    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "type"));
    if (!type) {
        LOG_WRN("Incoming message has no 'type' field — ignoring");
        cJSON_Delete(msg);
        return;
    }

    if (strcmp(type, "command") == 0) {
        /* Extract command name and optional payload, then dispatch */
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "command"));
        if (name) {
            const cJSON *payload_item = cJSON_GetObjectItem(msg, "payload");
            /* Serialise payload back to string for the handler.
             * Handlers that don't need the payload can ignore it.
             * NULL payload_json is valid — handlers must guard against it. */
            char *payload_json = payload_item
                ? cJSON_PrintUnformatted(payload_item) : NULL;
            dispatch_command(name, payload_json);
            if (payload_json) cJSON_FreeString(payload_json);
        }

    } else if (strcmp(type, "config") == 0) {
        /* Iterate each key in the config object and dispatch individually.
         * This means a push with 5 settings fires 5 separate handler calls,
         * each with a typed value — no JSON parsing in application code. */
        const cJSON *config_obj   = cJSON_GetObjectItem(msg, "config");
        const cJSON *version_item = cJSON_GetObjectItem(msg, "version");
        uint32_t version = cJSON_IsNumber(version_item)
            ? (uint32_t)version_item->valuedouble : 0;

        LOG_INF("OTA Config push received (v%u)", version);

        if (config_obj && cJSON_IsObject(config_obj)) {
            const cJSON *kv = NULL;
            cJSON_ArrayForEach(kv, config_obj) {
                /* kv->string is the key name; kv is the value node */
                dispatch_setting(kv->string, kv);
            }
        }

    } else {
        LOG_DBG("Unknown message type '%s' — ignoring", type);
    }

    cJSON_Delete(msg);
}

/* ── Transport callbacks (called from transport layer) ────────────────────
 *
 * Route through sdk_internal_event_handler which does SDK housekeeping first
 * then forwards to the user callback.
 */

void transport_on_connected(void)
{
    struct conexio_cloud_event evt = { .type = CONEXIO_CLOUD_EVT_CONNECTED };
    sdk_internal_event_handler(&evt);
}

void transport_on_disconnected(void)
{
    struct conexio_cloud_event evt = { .type = CONEXIO_CLOUD_EVT_DISCONNECTED };
    sdk_internal_event_handler(&evt);
}

/* ── Payload builder ──────────────────────────────────────────────────────
 *
 * Constructs the telemetry JSON string that is published to the cloud.
 *
 * Output format:
 * {
 *   "deviceId":  "351358815179730",
 *   "timestamp": "2026-06-10T14:30:00.123Z",
 *   "metrics": {
 *     "_rssi":       -72,    ← RSRP in dBm (auto, from modem)
 *     "_snr":        15,     ← SNR (auto, from modem, if non-zero)
 *     "_reboot_cnt": 4,      ← boot counter (auto, from NVS)
 *     "temperature": 22.5,   ← queued by application
 *     "humidity":    61.0    ← queued by application
 *   }
 * }
 *
 * Called by conexio_cloud_publish() in the background thread.
 * Returns a heap-allocated string — caller must cJSON_FreeString() it.
 */
static char *build_payload(void)
{
    /* ── Timestamp ────────────────────────────────────────────────────── */
    char timestamp[32];
    int64_t unix_ms;

    if (date_time_now(&unix_ms) == 0) {
        /* NTP has synced — build a proper ISO-8601 UTC timestamp */
        time_t t = (time_t)(unix_ms / 1000);
        struct tm *tm_val = gmtime(&t);
        snprintf(timestamp, sizeof(timestamp),
                 "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                 tm_val->tm_year + 1900, tm_val->tm_mon + 1, tm_val->tm_mday,
                 tm_val->tm_hour, tm_val->tm_min, tm_val->tm_sec,
                 (int)(unix_ms % 1000));
    } else {
        /* NTP not yet synced (shouldn't happen after init, but guard it).
         * A payload with epoch timestamp is rejected by the offline buffer
         * and skipped to avoid corrupting dashboard time-series charts. */
        strncpy(timestamp, "1970-01-01T00:00:00.000Z", sizeof(timestamp));
        LOG_WRN("NTP not synced — timestamp is epoch (1970)");
    }

    /* ── Build JSON ───────────────────────────────────────────────────── */
    cJSON *root    = cJSON_CreateObject();
    cJSON *metrics = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "deviceId",  g_device_id);
    cJSON_AddStringToObject(root, "timestamp", timestamp);
    cJSON_AddItemToObject(root, "metrics", metrics);

    /* ── Auto-metrics from the modem ─────────────────────────────────── */
    /*
     * modem_info_params_get() issues blocking AT commands (~10–50 ms).
     * We refresh every CONFIG_CONEXIO_CLOUD_MODEM_INFO_REFRESH publishes
     * (default 5, set in Kconfig) to reduce overhead without losing signal
     * quality visibility. Cached values are used in between refreshes.
     */
    static struct modem_param_info cached_modem_param;
    static int modem_refresh_counter = 0;

    if (modem_refresh_counter == 0) {
        if (modem_info_params_get(&cached_modem_param) != 0) {
            /* Failed — skip modem metrics this cycle, try again next time */
            goto skip_modem_metrics;
        }
    }
    modem_refresh_counter = (modem_refresh_counter + 1) %
                            CONFIG_CONEXIO_CLOUD_MODEM_INFO_REFRESH;

    /* ── Signal quality ───────────────────────────────────────────────
     * _rssi: RSRP in dBm.  Reference: RSRP_IDX_TO_DBM macro.
     *   idx < 0 → idx-140,  idx > 0 → idx-141.  Range: ~-44 to -156 dBm.
     * _snr:  SNR index.  SNR_IDX_TO_DB(x) = x-24 gives dB value.
     *   127 = SNR_UNAVAILABLE (modem could not measure). */
    cJSON_AddNumberToObject(metrics, "_rssi",
                            (double)cached_modem_param.network.rsrp.value);
    {
        int snr_val;
        if (modem_info_get_snr(&snr_val) == 0 && snr_val != SNR_UNAVAILABLE) {
            cJSON_AddNumberToObject(metrics, "_snr", (double)snr_val);
        }
    }

    /* ── LTE band (e.g. 3, 20, 28) ───────────────────────────────────
     * Indicates which frequency band the modem is using.
     * Useful for diagnosing coverage issues — some devices may only
     * get band 28 (rural) which has different propagation than band 3. */
    {
        uint8_t band = 0;
        if (modem_info_get_current_band(&band) == 0 && band != BAND_UNAVAILABLE) {
            cJSON_AddNumberToObject(metrics, "_lte_band", (double)band);
        }
    }

    /* ── Network operator name (e.g. "Telia SE", "AT&T") ─────────────
     * Sent ONCE per boot — operator never changes mid-session.
     * Saves ~30 bytes per payload vs. sending every publish. */
    {
        static bool g_operator_sent = false;
        if (!g_operator_sent) {
            char operator_buf[MODEM_INFO_SHORT_OP_NAME_SIZE] = {0};
            if (modem_info_get_operator(operator_buf, sizeof(operator_buf)) == 0
                && operator_buf[0] != '\0') {
                cJSON_AddStringToObject(metrics, "_operator", operator_buf);
                g_operator_sent = true;
            }
        }
    }

    /* ── Modem firmware version (e.g. "mfw_nrf9160_1.3.6") ──────────
     * Sent ONCE — on the first publish after each boot only.
     * The cloud stores the last-seen value per device, so there is no
     * need to repeat it on every payload.  Saves ~35 bytes per packet.
     * Same logic applies to _sdk_version below. */
    {
        static bool g_boot_metrics_sent = false;
        if (!g_boot_metrics_sent) {
            static char modem_fw_buf[MODEM_INFO_FWVER_SIZE] = {0};
            if (modem_fw_buf[0] == '\0') {
                modem_info_get_fw_version(modem_fw_buf, sizeof(modem_fw_buf));
            }
            if (modem_fw_buf[0] != '\0') {
                cJSON_AddStringToObject(metrics, "_modem_fw", modem_fw_buf);
            }
            /* _sdk_version — also first-publish-only */
            cJSON_AddStringToObject(metrics, "_sdk_version", CONEXIO_SDK_VERSION);
            g_boot_metrics_sent = true;
        }
    }

skip_modem_metrics:;  /* jump target if modem_info_params_get fails */

    /* ── Modem internal temperature ───────────────────────────────────
     * AT%XTEMP — modem die temperature in degrees Celsius.
     * Collected independently of the modem_params refresh cycle because
     * it uses a separate AT command and is always cheap to read.
     * -999 means unavailable (modem not in normal functional mode).
     * Alert threshold: >85°C triggers modem automatic power-off. */
#if defined(CONFIG_CONEXIO_CLOUD_PSM)
    {
        int temp = power_mgr_get_modem_temp();
        if (temp != -999) {
            cJSON_AddNumberToObject(metrics, "_modem_temp", (double)temp);
        }
    }
#endif

    /* ── Data usage counters (tx/rx kilobytes this session) ──────────
     * Accumulates since boot — always send so cloud sees the trend. */
    {
        int tx_kb = 0, rx_kb = 0;
        if (modem_info_get_connectivity_stats(&tx_kb, &rx_kb) == 0) {
            cJSON_AddNumberToObject(metrics, "_tx_kb", (double)tx_kb);
            cJSON_AddNumberToObject(metrics, "_rx_kb", (double)rx_kb);
        }
    }

    /* ── Publish-frequency strategy ──────────────────────────────────────
     *
     * Not all metrics need to be sent on every publish.  Sending static
     * or slowly-changing values on every packet wastes data for no benefit.
     *
     * Three tiers:
     *
     *   BOOT-ONCE  — value is fixed for the entire session; send only on
     *                the first publish after each boot.  The cloud stores
     *                the last-seen value per device and uses it for display.
     *                Metrics: _reboot_cnt, _reboot_reason, _lte_connect_ms,
     *                         _sdk_version, _modem_fw, _operator,
     *                         _lte_mode, _psm_tau_sec, _psm_active_sec,
     *                         _edrx_ms, _edrx_ptw_ms
     *
     *   SLOW       — value can change but rarely does (cell handover, band
     *                switch).  Send every CONFIG_CONEXIO_CLOUD_SLOW_METRIC_INTERVAL
     *                publishes (default 10 = every ~10 minutes at 60s interval).
     *                Metrics: _lte_band, _cell_id, _tac
     *
     *   EVERY      — value changes meaningfully every cycle; always include.
     *                Metrics: _rssi, _snr, _conn_loss, _reset_loop,
     *                         _tx_kb, _rx_kb, _modem_temp, _battery_mv,
     *                         application sensors
     *
     * Two static counters track where we are in the boot and slow cycles.
     */
    static bool    s_boot_sent    = false;  /* flips true after first publish */
    static uint16_t s_slow_tick   = 0;      /* counts up to SLOW_INTERVAL    */
    const  uint16_t slow_interval =
        CONFIG_CONEXIO_CLOUD_SLOW_METRIC_INTERVAL; /* Kconfig, default 10    */

    bool emit_boot = !s_boot_sent;
    bool emit_slow = (s_slow_tick == 0);

    /* ── BOOT-ONCE metrics ────────────────────────────────────────────── */
    if (emit_boot) {
        /* _reboot_cnt — increments only on reboot; constant this session */
        cJSON_AddNumberToObject(metrics, "_reboot_cnt", (double)g_reboot_cnt);

        /* _reboot_reason — set at boot from RESETREAS register; fixed */
        cJSON_AddStringToObject(metrics, "_reboot_reason", g_reboot_reason);

        /* _lte_connect_ms — measured once at boot registration */
        const struct conexio_lte_session_metrics *lm_boot =
            conexio_lte_get_session_metrics();
        if (lm_boot->connect_time_ms > 0) {
            cJSON_AddNumberToObject(metrics, "_lte_connect_ms",
                                    (double)lm_boot->connect_time_ms);
        }

        /* _lte_mode — stable once modem selects LTE-M or NB-IoT */
        if (lm_boot->lte_mode != 0) {
            cJSON_AddNumberToObject(metrics, "_lte_mode",
                                    (double)lm_boot->lte_mode);
        }

        /* PSM timers — granted once by network after registration */
        if (lm_boot->psm_tau_sec >= 0) {
            cJSON_AddNumberToObject(metrics, "_psm_tau_sec",
                                    (double)lm_boot->psm_tau_sec);
        }
        if (lm_boot->psm_active_time_sec >= 0) {
            cJSON_AddNumberToObject(metrics, "_psm_active_sec",
                                    (double)lm_boot->psm_active_time_sec);
        }
        if (lm_boot->edrx_interval_ms > 0) {
            cJSON_AddNumberToObject(metrics, "_edrx_ms",
                                    (double)lm_boot->edrx_interval_ms);
        }
        if (lm_boot->edrx_ptw_ms > 0) {
            cJSON_AddNumberToObject(metrics, "_edrx_ptw_ms",
                                    (double)lm_boot->edrx_ptw_ms);
        }

        /* String metrics that never change at runtime */
        {
            static char modem_fw_buf[MODEM_INFO_FWVER_SIZE] = {0};
            if (modem_fw_buf[0] == '\0') {
                modem_info_get_fw_version(modem_fw_buf, sizeof(modem_fw_buf));
            }
            if (modem_fw_buf[0] != '\0') {
                cJSON_AddStringToObject(metrics, "_modem_fw", modem_fw_buf);
            }
        }
        cJSON_AddStringToObject(metrics, "_sdk_version", CONEXIO_SDK_VERSION);
    }

    /* _operator: boot-once but retried until modem has attached */
    {
        static bool s_operator_sent = false;
        if (!s_operator_sent) {
            char operator_buf[MODEM_INFO_SHORT_OP_NAME_SIZE] = {0};
            if (modem_info_get_operator(operator_buf, sizeof(operator_buf)) == 0
                && operator_buf[0] != '\0') {
                cJSON_AddStringToObject(metrics, "_operator", operator_buf);
                s_operator_sent = true;
            }
        }
    }

    /* ── SLOW metrics (every N publishes) ─────────────────────────────── */
    if (emit_slow) {
        const struct conexio_lte_session_metrics *lm_slow =
            conexio_lte_get_session_metrics();

        /* _lte_band — changes only on cell handover or band reselection */
        {
            uint8_t band = 0;
            if (modem_info_get_current_band(&band) == 0
                && band != BAND_UNAVAILABLE) {
                cJSON_AddNumberToObject(metrics, "_lte_band", (double)band);
            }
        }

        /* _cell_id / _tac — change when device moves between cells */
        if (lm_slow->cell_id != 0xFFFFFFFF) {
            cJSON_AddNumberToObject(metrics, "_cell_id",
                                    (double)lm_slow->cell_id);
        }
        if (lm_slow->tac != 0xFFFFFFFF) {
            cJSON_AddNumberToObject(metrics, "_tac",
                                    (double)lm_slow->tac);
        }
    }

    /* Advance slow tick — wraps back to 0 to trigger next slow publish */
    s_slow_tick = (s_slow_tick + 1) % slow_interval;

    /* Mark boot metrics as sent after this payload is built */
    if (emit_boot) {
        s_boot_sent = true;
    }

    /* ── EVERY-PUBLISH metrics ────────────────────────────────────────── */

    /* _reset_loop — always emit when set (alert condition, must not be missed) */
    {
        const struct conexio_lte_session_metrics *lm = conexio_lte_get_session_metrics();
        if (lm->reset_loop_detected) {
            cJSON_AddNumberToObject(metrics, "_reset_loop", 1.0);
        }

        /* _conn_loss — accumulates; cloud needs latest value every publish */
        cJSON_AddNumberToObject(metrics, "_conn_loss",
                                (double)lm->connection_loss_count);
    }

#if defined(CONFIG_CONEXIO_CLOUD_AUTO_BATTERY)
    /* Battery voltage via modem AT%XVBAT — only available after modem init */
    {
        char bat_buf[16] = {0};
        if (modem_info_string_get(MODEM_INFO_BATTERY, bat_buf, sizeof(bat_buf)) > 0) {
            int bat_mv = atoi(bat_buf);
            if (bat_mv > 0) {
                cJSON_AddNumberToObject(metrics, "_battery_mv", (double)bat_mv);
            }
        }
    }
#endif /* CONFIG_CONEXIO_CLOUD_AUTO_BATTERY */

    /* ── Registered sensor callbacks ─────────────────────────────────── */
    for (int i = 0; i < sensor_count; i++) {
        if (!sensor_registry[i].used) continue;
        double val = sensor_registry[i].callback(sensor_registry[i].arg);
        /* NaN signals "skip this reading this cycle" (sensor unavailable) */
        if (!isnan(val)) {
            cJSON_AddNumberToObject(metrics, sensor_registry[i].name, val);
        }
    }

    /* ── Application metrics from the queue ──────────────────────────── */
    k_mutex_lock(&queue_mutex, K_FOREVER);
    for (int i = 0; i < CONFIG_CONEXIO_CLOUD_METRIC_QUEUE_SIZE; i++) {
        if (!metric_queue[i].used) continue;

        switch (metric_queue[i].type) {
        case 'n':
            cJSON_AddNumberToObject(metrics,
                                    metric_queue[i].name,
                                    metric_queue[i].num_val);
            break;
        case 's':
            cJSON_AddStringToObject(metrics,
                                    metric_queue[i].name,
                                    metric_queue[i].str_val);
            break;
        case 'b':
            cJSON_AddBoolToObject(metrics,
                                  metric_queue[i].name,
                                  metric_queue[i].bool_val);
            break;
        }
        /* Mark slot as free for the next publish cycle */
        metric_queue[i].used = false;
    }
    k_mutex_unlock(&queue_mutex);

    /* Serialise to a compact (no whitespace) JSON string.
     * cJSON_PrintUnformatted allocates from the heap — caller must free. */
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root); /* Always free the cJSON tree */
    return json;
}

/* ── Background thread function ───────────────────────────────────────────
 *
 * Runs continuously at the lowest application priority.
 * Uses g_sdk_interval_sec which can be updated at runtime via SET_INTERVAL.
 */
static void cloud_thread_fn(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

    int64_t last_publish_ms = 0;

    while (1) {

#if defined(CONFIG_CONEXIO_CLOUD_RETRY)
        retry_kick_watchdog();
#endif

#if defined(CONFIG_CONEXIO_CLOUD_PSM)
        if (power_mgr_is_psm_active()) {
            if (power_mgr_wake(30) != 0) {
                LOG_WRN("PSM wake timeout — skipping publish cycle");
                k_sleep(K_SECONDS(g_sdk_interval_sec));
                continue;
            }
        }
#endif

        /* ── Reconnect if needed ──────────────────────────────────────── */
        if (!transport_is_connected()) {
            int ret = transport_connect();
            if (ret) {
                LOG_WRN("transport_connect failed (%d) — retrying in 10 s", ret);
                k_sleep(K_SECONDS(10));
                continue;
            }
        }

        /* ── Drive MQTT event loop (500 ms window) ──────────────────── */
        transport_poll(K_MSEC(500));

        /* ── Periodic publish ─────────────────────────────────────────── *
         *
         * IMPORTANT: the SDK never publishes a metrics-only payload.
         * SDK auto-metrics (_rssi, _reboot_cnt, etc.) always ride alongside
         * application sensor data.  If the application has not queued any
         * data — no registered sensor callbacks AND no queued metrics —
         * the publish is skipped.
         *
         * This means the SDK publish interval (CONFIG_CONEXIO_CLOUD_INTERVAL_SEC)
         * acts as a *ceiling*, not a floor: the SDK will publish AT MOST once
         * per interval, but only when the application has something to send.
         *
         * For a device with a 5-minute sensor read cycle, set:
         *   CONFIG_CONEXIO_CLOUD_INTERVAL_SEC=300
         * The SDK fires every 300s, reads all registered sensor callbacks,
         * and publishes.  It never wakes the radio just for SDK metrics.
         *
         * If you call conexio_cloud_send_metric() manually from your own
         * thread on your own schedule, the SDK interval fires and picks up
         * whatever you queued.  Set interval to 0 to disable the background
         * publish entirely and call conexio_cloud_publish() yourself.
         */
        if (g_sdk_interval_sec > 0) {
            int64_t now = k_uptime_get();
            if (now - last_publish_ms >= (int64_t)g_sdk_interval_sec * 1000) {

                /* Guard: only publish if application has something to send.
                 *
                 * "has data" = at least one of:
                 *   a) a sensor callback is registered (will be called in
                 *      build_payload and may return a real value)
                 *   b) at least one metric is queued via send_metric()
                 *
                 * We do NOT skip on boot-once SDK metrics alone — those only
                 * exist to accompany application data, not to justify a wakeup.
                 */
                bool has_app_data = false;

                /* Check sensor registry */
                if (sensor_count > 0) {
                    has_app_data = true;
                }

                /* Check metric queue */
                if (!has_app_data) {
                    k_mutex_lock(&queue_mutex, K_FOREVER);
                    for (int i = 0; i < CONFIG_CONEXIO_CLOUD_METRIC_QUEUE_SIZE; i++) {
                        if (metric_queue[i].used) {
                            has_app_data = true;
                            break;
                        }
                    }
                    k_mutex_unlock(&queue_mutex);
                }

                if (!has_app_data) {
                    /* No application data — skip this publish cycle.
                     * SDK metrics will be included when the application
                     * next provides data to send. */
                    LOG_DBG("Publish skipped — no application data queued "
                            "(SDK metrics held back)");
                    last_publish_ms = now; /* advance timer to avoid busy-loop */
                } else {
                    int pub_ret = conexio_cloud_publish();

#if defined(CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER)
                    /* Buffer the payload if publish failed due to no connection */
                    if (pub_ret == -ENOTCONN) {
                        char *payload = build_payload();
                        if (payload) {
                            if (offline_buffer_push(payload, strlen(payload)) == 0) {
                                LOG_INF("Offline: buffered payload (%d pending)",
                                        offline_buffer_count());
                            }
                            cJSON_FreeString(payload);
                        }
                    }
#else
                    (void)pub_ret;
#endif
                    last_publish_ms = now;
                }
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API implementation
 * ═══════════════════════════════════════════════════════════════════════════*/

/*
 * conexio_cloud_register_command — register a named command handler.
 *
 * Must be called before conexio_cloud_init().
 * name is stored by pointer, not copied — use a string literal or a static
 * string that will outlive the SDK.
 */
int conexio_cloud_register_command(const char *name,
                                   conexio_command_handler_t handler,
                                   void *arg)
{
    if (!name || !handler) return -EINVAL;
    if (cmd_count >= CONFIG_CONEXIO_CLOUD_MAX_COMMANDS) {
        LOG_ERR("Command registry full (max %d) — increase "
                "CONFIG_CONEXIO_CLOUD_MAX_COMMANDS", CONFIG_CONEXIO_CLOUD_MAX_COMMANDS);
        return -ENOMEM;
    }
    cmd_registry[cmd_count].name    = name;
    cmd_registry[cmd_count].handler = handler;
    cmd_registry[cmd_count].arg     = arg;
    cmd_count++;
    LOG_DBG("Command registered: '%s' (%d/%d)",
            name, cmd_count, CONFIG_CONEXIO_CLOUD_MAX_COMMANDS);
    return 0;
}

/* conexio_cloud_register_setting_int — register an integer setting handler */
int conexio_cloud_register_setting_int(const char *key,
                                       conexio_int_setting_cb_t handler,
                                       void *arg)
{
    if (!key || !handler) return -EINVAL;
    if (setting_count >= CONFIG_CONEXIO_CLOUD_MAX_SETTINGS) return -ENOMEM;
    setting_registry[setting_count].key    = key;
    setting_registry[setting_count].type   = SETTING_INT;
    setting_registry[setting_count].cb_int = handler;
    setting_registry[setting_count].arg    = arg;
    setting_count++;
    LOG_DBG("Setting registered (int): '%s'", key);
    return 0;
}

/* conexio_cloud_register_setting_bool — register a boolean setting handler */
int conexio_cloud_register_setting_bool(const char *key,
                                        conexio_bool_setting_cb_t handler,
                                        void *arg)
{
    if (!key || !handler) return -EINVAL;
    if (setting_count >= CONFIG_CONEXIO_CLOUD_MAX_SETTINGS) return -ENOMEM;
    setting_registry[setting_count].key     = key;
    setting_registry[setting_count].type    = SETTING_BOOL;
    setting_registry[setting_count].cb_bool = handler;
    setting_registry[setting_count].arg     = arg;
    setting_count++;
    LOG_DBG("Setting registered (bool): '%s'", key);
    return 0;
}

/* conexio_cloud_register_setting_float — register a float setting handler */
int conexio_cloud_register_setting_float(const char *key,
                                         conexio_float_setting_cb_t handler,
                                         void *arg)
{
    if (!key || !handler) return -EINVAL;
    if (setting_count >= CONFIG_CONEXIO_CLOUD_MAX_SETTINGS) return -ENOMEM;
    setting_registry[setting_count].key      = key;
    setting_registry[setting_count].type     = SETTING_FLOAT;
    setting_registry[setting_count].cb_float = handler;
    setting_registry[setting_count].arg      = arg;
    setting_count++;
    LOG_DBG("Setting registered (float): '%s'", key);
    return 0;
}

/* conexio_cloud_register_setting_string — register a string setting handler */
int conexio_cloud_register_setting_string(const char *key,
                                          conexio_string_setting_cb_t handler,
                                          void *arg)
{
    if (!key || !handler) return -EINVAL;
    if (setting_count >= CONFIG_CONEXIO_CLOUD_MAX_SETTINGS) return -ENOMEM;
    setting_registry[setting_count].key       = key;
    setting_registry[setting_count].type      = SETTING_STRING;
    setting_registry[setting_count].cb_string = handler;
    setting_registry[setting_count].arg       = arg;
    setting_count++;
    LOG_DBG("Setting registered (string): '%s'", key);
    return 0;
}

/*
 * conexio_cloud_init — the main SDK entry point.
 *
 * Call this AFTER registering all command and setting handlers.
 * Blocks until LTE is registered and the background thread is running.
 *
 * On success the SDK background thread is running and the device will
 * attempt to connect to the cloud and start publishing.
 *
 * @param cb  Application event callback (may be NULL).
 * @return    0 on success, -EALREADY if already initialised,
 *            negative errno on any hardware or network failure.
 */
int conexio_cloud_init(conexio_cloud_event_cb_t cb)
{
    int ret = 0;

    if (g_initialised) {
        LOG_WRN("conexio_cloud_init() called more than once — ignoring");
        return -EALREADY;
    }

    /* Store the application callback — forwarded after SDK internal handling */
    user_cb = cb;
    memset(metric_queue, 0, sizeof(metric_queue));

    /* ── Step 1: Reboot counter ─────────────────────────────────────── */
    reboot_counter_init();

    /* ── Step 1b: Reboot reason ─────────────────────────────────────── */
    /* Must be called AFTER reboot_counter_init() because reboot_reason_init()
     * may write to NVS, which requires nvs_initialised = true. */
    reboot_reason_init();

    /* ── Step 2: Derive device ID from IMEI ────────────────────────── */
    modem_info_init();

    /* Enable connectivity statistics collection so _tx_kb/_rx_kb work.
     * This starts AT%XCONNSTAT=1 on the modem. Safe to call every boot;
     * the modem resets the counters when the modem library reinitialises. */
    if (modem_info_connectivity_stats_init() != 0) {
        LOG_WRN("modem_info_connectivity_stats_init failed — _tx_kb/_rx_kb unavailable");
    }

    struct modem_param_info mp;
    if (modem_info_params_get(&mp) == 0) {
        char imei[16] = {0};
        strncpy(imei, mp.device.imei.value_string, sizeof(imei) - 1);
        for (int i = (int)strlen(imei) - 1; i >= 0; i--) {
            if (imei[i] <= ' ') imei[i] = '\0'; else break;
        }
        /* Device ID is the bare 15-digit IMEI, e.g. "351358815179730".
         * No prefix so it's clean, universally recognisable, and matches
         * the imei-index GSI key in the cloud DynamoDB devices table. */
        strncpy(g_device_id, imei, sizeof(g_device_id) - 1);
    } else {
        LOG_WRN("IMEI unavailable — using fallback device ID");
        strncpy(g_device_id, "000000000000000", sizeof(g_device_id) - 1);
    }
    LOG_INF("Device ID: %s", g_device_id);
    LOG_INF("Registered: %d command(s), %d setting(s)", cmd_count, setting_count);

    /* ── Step 3: Retry + watchdog init ─────────────────────────────── */
#if defined(CONFIG_CONEXIO_CLOUD_RETRY)
    struct retry_config retry_cfg = {
        .base_sec        = CONFIG_CONEXIO_CLOUD_RETRY_BASE_SEC,
        .max_sec         = CONFIG_CONEXIO_CLOUD_RETRY_MAX_SEC,
        .max_attempts    = CONFIG_CONEXIO_CLOUD_RETRY_MAX_ATTEMPTS,
        .wdt_timeout_sec = CONFIG_CONEXIO_CLOUD_WATCHDOG_TIMEOUT_SEC,
    };
    retry_init(&retry_cfg);
    LOG_INF("Retry: base=%ds max=%ds attempts=%d wdt=%ds",
            retry_cfg.base_sec, retry_cfg.max_sec,
            retry_cfg.max_attempts, retry_cfg.wdt_timeout_sec);
#endif

    /* ── Step 4: Offline buffer init ────────────────────────────────── */
#if defined(CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER)
    {
        int ret = offline_buffer_init();
        if (ret) {
            LOG_WRN("Offline buffer init failed (%d) — buffering disabled", ret);
        } else {
            LOG_INF("Offline buffer: %d/%d payloads pending",
                    offline_buffer_count(),
                    CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER_SIZE);
        }
    }
#endif

    /* ── Step 5: Register built-in commands ─────────────────────────── */
    /* REBOOT — always built-in (safe no matter what hardware) */
    conexio_cloud_register_command("REBOOT", builtin_on_reboot, NULL);

    /* SET_INTERVAL — built-in; updates g_sdk_interval_sec at runtime */
    conexio_cloud_register_command("SET_INTERVAL", builtin_on_set_interval, NULL);

#if defined(CONFIG_CONEXIO_CLOUD_FOTA)
    /* FIRMWARE_UPDATE — built-in when FOTA is enabled */
    conexio_cloud_register_command("FIRMWARE_UPDATE", builtin_on_firmware_update, NULL);
#endif

#if defined(CONFIG_CONEXIO_CLOUD_AUTO_INTERVAL_SETTING)
    /* telemetryIntervalSec — built-in OTA Config setting.
     * Updates both the SDK publish interval and g_sdk_interval_sec.
     * When enabled the application does not need its own handler for this key. */
    conexio_cloud_register_setting_int("telemetryIntervalSec",
                                       builtin_on_interval_setting, NULL);
#endif

    /* ── Step 6: Connect LTE (if SDK manages it) ────────────────────── */
#if defined(CONFIG_CONEXIO_CLOUD_MANAGE_LTE)
    ret = conexio_lte_connect(CONFIG_CONEXIO_CLOUD_LTE_TIMEOUT_SEC);
    if (ret) {
        LOG_ERR("LTE connection failed (%d)", ret);
        dispatch_error(ret);
        return ret;
    }

    /* NTP time sync — use event callback instead of blind k_sleep().
     * Waits up to CONFIG_CONEXIO_CLOUD_NTP_TIMEOUT_SEC for SNTP response.
     * Falls back gracefully if NTP is unavailable (offline buffer will
     * skip payloads with epoch timestamp automatically). */
    date_time_register_handler(ntp_event_handler);
    date_time_update_async(NULL);
    {
        int ntp_ret = k_sem_take(&ntp_ready_sem,
                                  K_SECONDS(CONFIG_CONEXIO_CLOUD_NTP_TIMEOUT_SEC));
        if (ntp_ret == 0) {
            LOG_INF("NTP sync complete");
        } else {
            LOG_WRN("NTP sync timed out after %ds — timestamps may be inaccurate",
                    CONFIG_CONEXIO_CLOUD_NTP_TIMEOUT_SEC);
        }
    }
#endif

    /* ── Step 7: Fetch cloud config from Conexio config service ─────── */
    struct conexio_cloud_config_t cloud_cfg;
    ret = config_fetch(g_device_id, &cloud_cfg);
    if (ret) {
        LOG_ERR("config_fetch() failed (%d)", ret);
        dispatch_error(ret);
        return ret;
    }

    /* ── Step 8: Provision TLS credentials ─────────────────────────── */
    ret = cert_store_provision_from_config(&cloud_cfg);
    if (ret) {
        LOG_ERR("cert_store_provision_from_config() failed (%d)", ret);
        dispatch_error(ret);
        return ret;
    }

    /* ── Step 9: Initialise transport ──────────────────────────────── */
    ret = transport_init_with_config(g_device_id, &cloud_cfg);
    if (ret) {
        LOG_ERR("transport_init_with_config() failed (%d)", ret);
        return ret;
    }

    /* ── Step 10: Power management init ─────────────────────────────── */
#if defined(CONFIG_CONEXIO_CLOUD_PSM)
    {
        struct power_mgr_config pwr_cfg = {
            .psm_enable          = !IS_ENABLED(CONFIG_CONEXIO_CLOUD_EDRX),
            .psm_tau_sec         = CONFIG_CONEXIO_CLOUD_PSM_TAU_SEC,
            .psm_active_time_sec = CONFIG_CONEXIO_CLOUD_PSM_ACTIVE_TIME_SEC,
            .edrx_enable         = IS_ENABLED(CONFIG_CONEXIO_CLOUD_EDRX),
        };
        power_mgr_init(&pwr_cfg);
    }
#endif

    /* ── Step 11: FOTA init ─────────────────────────────────────────── */
#if defined(CONFIG_CONEXIO_CLOUD_FOTA)
    fota_init(g_device_id, sdk_fota_event_handler);
    /* Post-FOTA boot: confirm the new image so MCUboot won't roll back */
    if (boot_is_img_confirmed() == 0) {
        LOG_INF("Post-FOTA boot — confirming new firmware");
        fota_confirm();
    }
#endif

    /* ── Step 12: Spawn background thread ──────────────────────────── */
    k_thread_create(&cloud_thread_data, cloud_stack,
                    K_THREAD_STACK_SIZEOF(cloud_stack),
                    cloud_thread_fn,
                    NULL, NULL, NULL,
                    K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);
    k_thread_name_set(&cloud_thread_data, "conexio_cloud");

    g_initialised = true;
    LOG_INF("SDK initialised — PSM=%s Buffer=%s FOTA=%s Retry=%s",
            IS_ENABLED(CONFIG_CONEXIO_CLOUD_PSM)            ? "ON" : "OFF",
            IS_ENABLED(CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER) ? "ON" : "OFF",
            IS_ENABLED(CONFIG_CONEXIO_CLOUD_FOTA)           ? "ON" : "OFF",
            IS_ENABLED(CONFIG_CONEXIO_CLOUD_RETRY)          ? "ON" : "OFF");
    return 0;
}

    /* ── Step 1: Reboot counter ─────────────────────────────────────── */
/* ── Public helpers ───────────────────────────────────────────────────────*/

/** Manually trigger a connection (e.g. after waking from PSM). */
int conexio_cloud_connect(void)    { return transport_connect(); }

/** Disconnect from the cloud. */
int conexio_cloud_disconnect(void) { return transport_disconnect(); }

/*
 * conexio_cloud_send_metric — queue a numeric metric for the next publish.
 *
 * Thread-safe.  If the same name is already queued, its value is overwritten
 * (only the latest value is published per interval).  If the queue is full,
 * returns -ENOMEM — increase CONFIG_CONEXIO_CLOUD_METRIC_QUEUE_SIZE.
 */
int conexio_cloud_send_metric(const char *name, double value)
{
    if (!name) return -EINVAL;

    k_mutex_lock(&queue_mutex, K_FOREVER);
    int slot = -1;
    for (int i = 0; i < CONFIG_CONEXIO_CLOUD_METRIC_QUEUE_SIZE; i++) {
        /* Check for an existing entry with the same name (update-in-place) */
        if (metric_queue[i].used && strcmp(metric_queue[i].name, name) == 0) {
            slot = i; break;
        }
        /* Remember the first free slot in case we need to create a new entry */
        if (!metric_queue[i].used && slot == -1) slot = i;
    }
    if (slot == -1) {
        k_mutex_unlock(&queue_mutex);
        LOG_ERR("Metric queue full — increase CONFIG_CONEXIO_CLOUD_METRIC_QUEUE_SIZE");
        return -ENOMEM;
    }
    strncpy(metric_queue[slot].name, name, MAX_METRIC_NAME - 1);
    metric_queue[slot].type    = 'n';
    metric_queue[slot].num_val = value;
    metric_queue[slot].used    = true;
    k_mutex_unlock(&queue_mutex);
    return 0;
}

/* conexio_cloud_send_metric_str — queue a string metric */
int conexio_cloud_send_metric_str(const char *name, const char *value)
{
    if (!name || !value) return -EINVAL;
    k_mutex_lock(&queue_mutex, K_FOREVER);
    int slot = -1;
    for (int i = 0; i < CONFIG_CONEXIO_CLOUD_METRIC_QUEUE_SIZE; i++) {
        if (metric_queue[i].used && strcmp(metric_queue[i].name, name) == 0)
            { slot = i; break; }
        if (!metric_queue[i].used && slot == -1) slot = i;
    }
    if (slot == -1) { k_mutex_unlock(&queue_mutex); return -ENOMEM; }
    strncpy(metric_queue[slot].name,    name,  MAX_METRIC_NAME - 1);
    strncpy(metric_queue[slot].str_val, value, 63);
    metric_queue[slot].type = 's';
    metric_queue[slot].used = true;
    k_mutex_unlock(&queue_mutex);
    return 0;
}

/* conexio_cloud_send_metric_bool — queue a boolean metric */
int conexio_cloud_send_metric_bool(const char *name, bool value)
{
    if (!name) return -EINVAL;
    k_mutex_lock(&queue_mutex, K_FOREVER);
    int slot = -1;
    for (int i = 0; i < CONFIG_CONEXIO_CLOUD_METRIC_QUEUE_SIZE; i++) {
        if (metric_queue[i].used && strcmp(metric_queue[i].name, name) == 0)
            { slot = i; break; }
        if (!metric_queue[i].used && slot == -1) slot = i;
    }
    if (slot == -1) { k_mutex_unlock(&queue_mutex); return -ENOMEM; }
    strncpy(metric_queue[slot].name, name, MAX_METRIC_NAME - 1);
    metric_queue[slot].type     = 'b';
    metric_queue[slot].bool_val = value;
    metric_queue[slot].used     = true;
    k_mutex_unlock(&queue_mutex);
    return 0;
}

/*
 * conexio_cloud_publish — immediately publish all queued metrics.
 *
 * Called automatically by the background thread every INTERVAL_SEC.
 * Can also be called directly by the application for event-driven publishing
 * (e.g. after a sensor alarm).
 *
 * Returns -ENOTCONN if not connected (metrics remain in the queue for the
 * next attempt — useful with the offline buffer in sdk-sample-app-advanced).
 */
int conexio_cloud_publish(void)
{
    if (!transport_is_connected()) return -ENOTCONN;

    char *payload = build_payload();
    if (!payload) {
        LOG_ERR("build_payload() returned NULL — out of heap memory?");
        return -ENOMEM;
    }

    int ret = transport_publish(payload, strlen(payload));
    cJSON_FreeString(payload); /* Always free the heap-allocated JSON string */

    if (ret == 0) {
        /* Route PUBLISHED through the SDK internal handler so PSM sleep fires */
        struct conexio_cloud_event evt = { .type = CONEXIO_CLOUD_EVT_PUBLISHED };
        sdk_internal_event_handler(&evt);
    } else {
        LOG_WRN("transport_publish failed (%d)", ret);
    }
    return ret;
}

/** Returns true if the transport is currently connected to the cloud. */
bool conexio_cloud_is_connected(void)  { return transport_is_connected(); }
const char *conexio_cloud_device_id(void) { return g_device_id; }

/** Returns the current publish interval in seconds (may differ from Kconfig
 *  default if SET_INTERVAL or telemetryIntervalSec OTA Config was applied). */
int conexio_cloud_get_interval_sec(void) { return g_sdk_interval_sec; }

/** Returns the SDK semantic version string, e.g. "2.1.0". */
const char *conexio_cloud_version(void) { return CONEXIO_SDK_VERSION; }

/**
 * conexio_cloud_register_sensor — register a sensor reading callback.
 *
 * The SDK calls this function before each telemetry publish and adds the
 * returned value as a metric.  Applications no longer need to call
 * conexio_cloud_send_metric() in their main loop.
 *
 * Return NAN from the callback to skip a reading for that cycle.
 */
int conexio_cloud_register_sensor(const char *name,
                                   conexio_sensor_read_cb_t callback,
                                   void *arg)
{
    if (!name || !callback) return -EINVAL;
    /* Registration must happen before init — sensors are called from the
     * background thread which only exists after conexio_cloud_init(). */
    __ASSERT(!g_initialised,
             "conexio_cloud_register_sensor() called after init — "
             "register all sensors before conexio_cloud_init()");
    if (g_initialised) {
        LOG_ERR("register_sensor('%s') called after init — ignored", name);
        return -EPERM;
    }
    if (sensor_count >= CONFIG_CONEXIO_CLOUD_MAX_SENSORS) {
        LOG_ERR("Sensor registry full — increase CONFIG_CONEXIO_CLOUD_MAX_SENSORS");
        return -ENOMEM;
    }
    strncpy(sensor_registry[sensor_count].name, name, MAX_SENSOR_NAME - 1);
    sensor_registry[sensor_count].callback = callback;
    sensor_registry[sensor_count].arg      = arg;
    sensor_registry[sensor_count].used     = true;
    sensor_count++;
    LOG_DBG("Sensor registered: '%s' (%d/%d)",
            name, sensor_count, CONFIG_CONEXIO_CLOUD_MAX_SENSORS);
    return 0;
}

#if defined(CONFIG_CONEXIO_CLOUD_FOTA)
/** Let the application override the built-in FOTA progress logger. */
void conexio_cloud_set_fota_cb(fota_event_cb_t cb) { g_fota_user_cb = cb; }
#endif
