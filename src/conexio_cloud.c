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
#include <zephyr/random/random.h>     /* sys_rand32_get() for session run ID */
#include <string.h>
#include <stdio.h>
#include <inttypes.h>                 /* PRId64 for schedule watchdog logs  */
#include <limits.h>                   /* INT_MAX for g_interval_max_sec default */
#include <cJSON.h>
#include <cJSON_os.h>
#include <modem/nrf_modem_lib.h>
#include <modem/modem_info.h>
#include <date_time.h>                /* NTP time sync + ISO-8601 timestamp */
#include <math.h>                     /* isnan() for sensor callback return */

#include "conexio_cloud/conexio_cloud.h"  /* Public API (included by app)  */
#include "transport.h"               /* Internal transport interface       */
#include "cert_store.h"              /* TLS credential management          */
#include "lte.h"                     /* LTE connection helper               */
#include "config_fetch.h"            /* Phase 2: runtime config fetch      */

/* App firmware version from the application's VERSION file.
 * Generated by Zephyr's cmake/modules/version.cmake into
 * <build>/zephyr/include/generated/zephyr/app_version.h.
 * Falls back gracefully if the application has no VERSION file. */
#if __has_include(<app_version.h>)
#  include <app_version.h>
#  define CONEXIO_APP_FW_VERSION APP_VERSION_STRING
#else
#  define CONEXIO_APP_FW_VERSION "unknown"
#endif

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
#if defined(CONFIG_CONEXIO_CLOUD_LOG_STREAM)
#include "log_stream.h"
#endif

/* SDK semantic version — reported in every telemetry payload as _sdk_version */
#define CONEXIO_SDK_VERSION "2.3.0"

LOG_MODULE_REGISTER(conexio_cloud, LOG_LEVEL_INF);

/* ── SDK connection status ────────────────────────────────────────────────── */
/* ── Session Run ID ───────────────────────────────────────────────────────────
 * Generated once at boot from a hardware RNG. Included in every telemetry
 * payload as _session_id so the cloud can group all data from one power-on
 * session, correlate reconnects, and distinguish "device rebooted" from
 * "device just moved cells". Lighter than the NVS-backed reboot counter for
 * per-session correlation. Format: 8 hex chars (32-bit random value).
 */
/* ── Session Run ID ───────────────────────────────────────────────────────────
 * Generated once at boot from a hardware RNG. Included in every telemetry
 * payload as _session_id so the cloud can group all data from one power-on
 * session and correlate reconnects across MQTT disconnects. Lighter than the
 * NVS-backed reboot counter for per-session correlation — no flash write needed.
 * Format: 8 lowercase hex chars representing a 32-bit random value.
 */
static char g_session_id[9] = {0};   /* "aabbccdd\0" — filled in init */

static enum conexio_cloud_status g_sdk_status = CONEXIO_CLOUD_STATUS_INIT;
static K_SEM_DEFINE(g_connected_sem, 0, 1);  /* given on MQTT CONNACK */

/* ── Publish reliability counters ────────────────────────────────────────── */
/* Accumulated since boot — published as _publish_success_count and
 * _publish_fail_count on every telemetry payload so the cloud can
 * compute a per-device publish reliability percentage. */
static uint32_t g_publish_success_count = 0;
static uint32_t g_publish_fail_count    = 0;

/* ── Per-topic sequence numbers ───────────────────────────────────────────
 * Monotonically increasing per topic. Included in every payload so the
 * cloud can detect gaps (missed messages) and reorder out-of-order delivery
 * within a single topic stream. Reset to 0 on device reboot (RAM-only).
 */
static uint32_t g_seq_telemetry    = 0;
static uint32_t g_seq_diagnostics  = 0;
static uint32_t g_seq_location     = 0;
static uint32_t g_seq_logs         = 0;

#if defined(CONFIG_CONEXIO_CLOUD_BATTERY_METRICS)
#include <zephyr/drivers/sensor.h>
#include <nrf_fuel_gauge.h>  /* nrf_fuel_gauge_process() — must be init'd before use */

/* nPM1300 battery SOC and drain rate tracking ──────────────────────────────
 * g_last_soc_pct:    SOC% at the previous publish (-1 = not yet read).
 * g_last_pub_time_ms: k_uptime_get() at the previous publish.
 * We track these between publishes to compute instantaneous drain rate. */
static float    g_last_soc_pct      = -1.0f;  /* float — preserves sub-percent precision */
static int64_t  g_last_pub_time_ms  =  0;
static double   g_last_battery_mv   = NAN; /* voltage from most recent fuel gauge read */

/* Flag set before an intentional pre-PSM disconnect so the DISCONNECTED
 * event handler knows to skip retry_on_failure(). Cleared after use. */
static bool g_intentional_disconnect = false;

/* ── Cached RSRP — updated asynchronously via %CESQ notification ─────────
 * The modem pushes a %CESQ notification whenever it measures a new signal
 * quality value. We cache the latest valid RSRP index here so build_payload()
 * always has a fresh value, even right after PSM wake when AT+CESQ
 * (polled via modem_info_params_get) may still return 255 (not measured yet).
 * INT16_MIN = "never received a valid reading" sentinel. */
#define RSRP_NOT_KNOWN_IDX  255
#define RSRP_CACHE_INVALID  INT16_MIN
static int16_t  g_cached_rsrp_idx  = RSRP_CACHE_INVALID;

/* Called by modem_info whenever the modem pushes a fresh %CESQ notification.
 * The value is the raw RSRP index (not dBm). 255 = not measured — discard. */
static void on_rsrp_notification(char rsrp_value)
{
    uint8_t idx = (uint8_t)rsrp_value;
    if (idx != RSRP_NOT_KNOWN_IDX) {
        g_cached_rsrp_idx = (int16_t)idx;
        LOG_DBG("RSRP updated: idx=%d", idx);
    }
}

#if defined(CONFIG_CONEXIO_CLOUD_BATTERY_METRICS)
/** Returns the battery voltage (mV) cached by the last fuel gauge read.
 *  Returns NAN if no fuel gauge read has occurred yet this session.
 *  Used by read_battery_mv() in main.c to avoid a second sensor_sample_fetch. */
double conexio_cloud_get_last_battery_mv(void) { return g_last_battery_mv; }
#endif

/* Device handle — obtained lazily on first use. */
static const struct device *g_pmic_charger_dev;

static void battery_metrics_init(void)
{
    /* Use DT-based device lookup — device_get_binding() is deprecated
     * in NCS v3.2.1 and may return NULL even when the device exists. */
    g_pmic_charger_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(pmic_charger));
    if (!g_pmic_charger_dev || !device_is_ready(g_pmic_charger_dev)) {
        LOG_WRN("battery_metrics: pmic_charger device not ready — "
                "_battery_soc_pct and _battery_drain_pct_hr unavailable");
        g_pmic_charger_dev = NULL;
    }
}

/* ── battery_read_soc ────────────────────────────────────────────────────
 * Read battery state-of-charge % using the nRF Fuel Gauge library.
 *
 * The nPM1300 hardware does NOT compute SOC internally. SOC comes from
 * nrf_fuel_gauge_process() — a Coulomb-counting algorithm that must be
 * called with fresh V/I/T readings on every sample. We cannot simply
 * read SENSOR_CHAN_GAUGE_STATE_OF_CHARGE from the driver.
 *
 * The fuel gauge must have been initialised by calling fuel_gauge_init()
 * (done in main.c before conexio_cloud_init()). This function just
 * drives the algorithm forward on each publish cycle.
 *
 * Also returns charging state via *is_charging (true = plugged in).
 * Returns SOC 0.0–100.0 on success, -1.0 on any read error.
 */
static float battery_read_soc(bool *is_charging)
{
    if (!g_pmic_charger_dev) return -1.0f;

    /* Single sensor_sample_fetch per cycle — reads all channels atomically */
    if (sensor_sample_fetch(g_pmic_charger_dev) < 0) return -1.0f;

    struct sensor_value v_val, i_val, t_val;

    if (sensor_channel_get(g_pmic_charger_dev,
                           SENSOR_CHAN_GAUGE_VOLTAGE, &v_val) < 0) return -1.0f;
    if (sensor_channel_get(g_pmic_charger_dev,
                           SENSOR_CHAN_GAUGE_TEMP, &t_val) < 0) {
        /* NTC not connected (thermistor-ohms = 0 in DTS) — driver returns
         * -ENOTSUP for GAUGE_TEMP. Use 25°C as a safe default for the
         * fuel gauge algorithm. SOC accuracy is slightly reduced but the
         * algorithm remains stable. */
        t_val.val1 = 25;
        t_val.val2 = 0;
    }
    if (sensor_channel_get(g_pmic_charger_dev,
                           SENSOR_CHAN_GAUGE_AVG_CURRENT, &i_val) < 0) return -1.0f;

    float voltage = (float)v_val.val1 + (float)v_val.val2 / 1000000.0f;
    float temp    = (float)t_val.val1 + (float)t_val.val2 / 1000000.0f;
    /* Zephyr: negative current = discharging.
     * nRF Fuel Gauge expects the opposite sign convention: negate here. */
    float current = -((float)i_val.val1 + (float)i_val.val2 / 1000000.0f);

    /* Cache voltage in mV so read_battery_mv() can use it without a
     * second sensor_sample_fetch() on the same publish cycle. */
    g_last_battery_mv = (double)voltage * 1000.0;

    /* Determine charging state before sign flip:
     * Zephyr reports positive val1 for charging current */
    if (is_charging) {
        *is_charging = (i_val.val1 > 0) ||
                       (i_val.val1 == 0 && i_val.val2 > 0);
    }

    /* Drive the Coulomb-counting algorithm.
     * k_uptime_delta_32() returns ms since last call — convert to seconds. */
    static int64_t last_sample_ms = 0;
    int64_t now_ms = k_uptime_get();
    float delta_sec = (last_sample_ms > 0)
        ? (float)(now_ms - last_sample_ms) / 1000.0f
        : 0.0f;  /* first call — no delta yet, fuel gauge uses init state */
    last_sample_ms = now_ms;

    float soc = nrf_fuel_gauge_process(voltage, current, temp, delta_sec, NULL);

    LOG_DBG("battery: V=%.3fV I=%.3fA T=%.1fC SOC=%.1f%% delta=%.1fs",
            (double)voltage, (double)(-current), (double)temp,
            (double)soc, (double)delta_sec);

    return soc;
}
#endif /* CONFIG_CONEXIO_CLOUD_BATTERY_METRICS */

enum conexio_cloud_status conexio_cloud_get_status(void) { return g_sdk_status; }

int conexio_cloud_wait_connected(int32_t timeout_ms)
{
    if (g_sdk_status == CONEXIO_CLOUD_STATUS_CONNECTED) return 0;
    k_timeout_t t = (timeout_ms < 0) ? K_FOREVER : K_MSEC(timeout_ms);
    int ret = k_sem_take(&g_connected_sem, t);
    return (ret == 0) ? 0 : -ETIMEDOUT;
}

/* ── utc_tm_to_epoch ─────────────────────────────────────────────────────
 * Convert a struct tm expressed in UTC to seconds since the Unix epoch.
 *
 * Replaces timegm() which is not available in Zephyr's newlib toolchain
 * for embedded targets. Uses the proleptic Gregorian calendar algorithm —
 * no libc dependency, safe on any C99 compiler.
 *
 * @param t  struct tm with UTC fields (tm_year, tm_mon, tm_mday,
 *           tm_hour, tm_min, tm_sec). Other fields are ignored.
 * @return   Seconds since 1970-01-01 00:00:00 UTC.
 */
static int64_t utc_tm_to_epoch(const struct tm *t)
{
    /* Days from epoch to 1 March of the given year (Gregorian reform) */
    int y = t->tm_year + 1900 - (t->tm_mon < 2 ? 1 : 0);
    int m = t->tm_mon + 1;           /* 1-12 */
    if (m <= 2) m += 12;

    /* Julian Day Number for 1 March, then adjust back to Jan 1 */
    int64_t days = (int64_t)365 * y + y / 4 - y / 100 + y / 400
                 + (153 * m - 457) / 5
                 + t->tm_mday - 719469;  /* offset to Unix epoch */

    return days * 86400LL
         + t->tm_hour * 3600LL
         + t->tm_min  * 60LL
         + t->tm_sec;
}

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
    /* Optional SDK-enforced range — only used by _with_range variants.
     * has_range=false means no range check (plain register_setting_*). */
    bool    has_range;
    double  range_min;   /* double covers both int32 and float ranges */
    double  range_max;
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
    char   category;      /* TOPIC_CAT_* — which topic this metric routes to */
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
 *   0x0003 = schedule watchdog active flag  (this file)
 *   0x0004 = schedule watchdog record       (this file)
 *   0x0005 = publish interval override      (this file) ← new
 *   0x0010-0x0012 = offline buffer metadata
 *   0x2000+ = offline buffer entries
 */
#define REBOOT_CNT_NVS_ID        0x0001U
#define REBOOT_REASON_NVS_ID     0x0002U
#define SCHED_WDT_ACTIVE_NVS_ID  0x0003U
#define SCHED_WDT_RECORD_NVS_ID  0x0004U
/* Persists the runtime publish interval override (SET_INTERVAL / telemetryIntervalSec).
 * Stored as int32_t. Written on every successful interval change.
 * Read on boot to restore the last OTA-configured interval. */
#define INTERVAL_OVERRIDE_NVS_ID 0x0005U

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

/* SET_INTERVAL — updates the SDK-managed publish interval */
static int g_sdk_interval_sec = CONFIG_CONEXIO_CLOUD_INTERVAL_SEC;

/* Application-configurable interval limits.
 * Set via conexio_cloud_register_interval() before init.
 * Defaults: min=10s, max=INT_MAX (no upper cap). */
static int g_interval_min_sec = 10;
static int g_interval_max_sec = INT_MAX;

/* reboot_counter_init — called once from conexio_cloud_init() */
static void reboot_counter_init(void)
{
#if defined(CONFIG_NVS)
    g_reboot_cnt = load_and_increment_reboot_count();
    LOG_INF("Reboot counter: %u (persisted in NVS)", g_reboot_cnt);

    /* ── Restore persisted publish interval ────────────────────────────────
     * If SET_INTERVAL or telemetryIntervalSec was applied in a previous
     * session, restore it now so the device wakes at the same cadence after
     * a reboot (FOTA, watchdog, power cycle, etc.).
     * -ENOENT on first boot is normal — stay at CONFIG_CONEXIO_CLOUD_INTERVAL_SEC. */
    {
        int32_t stored_interval = 0;
        ssize_t r = nvs_read(&reboot_nvs, INTERVAL_OVERRIDE_NVS_ID,
                             &stored_interval, sizeof(stored_interval));
        if (r > 0 && stored_interval >= 1) {
            /* Apply bounds from conexio_cloud_register_interval() if already called.
             * g_interval_min/max may still be defaults (10 / INT_MAX) at this
             * point — the application calls register_interval() BEFORE init,
             * so the values are available here. */
            if (stored_interval >= g_interval_min_sec &&
                stored_interval <= g_interval_max_sec) {
                g_sdk_interval_sec = (int)stored_interval;
                LOG_INF("Publish interval restored from NVS: %ds", g_sdk_interval_sec);
            } else {
                /* Stored value violates current app limits — clear it and use default */
                LOG_WRN("NVS interval %ds outside registered bounds [%d, %d] "
                        "— resetting to Kconfig default %ds",
                        (int)stored_interval, g_interval_min_sec,
                        g_interval_max_sec, CONFIG_CONEXIO_CLOUD_INTERVAL_SEC);
                nvs_delete(&reboot_nvs, INTERVAL_OVERRIDE_NVS_ID);
            }
        }
    }

    /* ── Session Run ID ────────────────────────────────────────────────────
     * Generate a random 32-bit value using the hardware RNG. Formatted as
     * 8 lowercase hex chars. Different on every boot — no NVS write needed.
     */
    snprintf(g_session_id, sizeof(g_session_id), "%08x", sys_rand32_get());
    LOG_INF("Session ID: %s", g_session_id);
#else
    g_reboot_cnt = 0;
    LOG_DBG("Reboot counter: 0 (NVS disabled — not persistent)");
#endif
}

/* ── Schedule watchdog ────────────────────────────────────────────────────
 *
 * Level 2 autonomous schedule execution.
 *
 * When a start command arrives that contains stopCommand + stopAt fields,
 * the SDK:
 *   1. Dispatches the start command immediately (e.g. LED_ON).
 *   2. Stores the stop info (command name, payload, stopAt epoch) in NVS
 *      so it survives a reboot.
 *   3. Arms a k_timer for (stopAt - now).  When the timer fires it
 *      dispatches the stop command locally — no cloud connection required.
 *   4. Clears the NVS record after the stop command executes.
 *
 * On boot, sched_watchdog_boot_check() is called from conexio_cloud_init():
 *   - If a watchdog record exists AND stopAt is still in the future,
 *     the timer is re-armed for the remaining duration.
 *   - If stopAt has already passed, the stop command is executed immediately
 *     (device may have been powered off during the schedule window).
 *   - If no record exists, nothing happens.
 *
 * The optional user callback conexio_cloud_register_schedule_cb() is invoked
 * for three events: SCHEDULE_STARTED, SCHEDULE_STOPPED, SCHEDULE_EXPIRED.
 *
 * NVS layout (requires CONFIG_NVS=y):
 *   0x0003 — uint8_t active flag (1 = watchdog armed)
 *   0x0004 — struct sched_watchdog_nvs record
 *
 * Thread safety:
 *   The timer callback runs in the system work queue context.
 *   dispatch_command() is safe to call from any context.
 */

/* Max command name length stored in the watchdog record */
#define SCHED_WDT_CMD_MAX  32
/* Max payload JSON stored in the watchdog record */
#define SCHED_WDT_PLD_MAX  128

/* NVS-persisted watchdog record */
struct sched_watchdog_nvs {
    char    stop_command[SCHED_WDT_CMD_MAX];   /* e.g. "LED_OFF"          */
    char    stop_payload[SCHED_WDT_PLD_MAX];   /* serialised JSON payload  */
    int64_t stop_at_ms;                         /* Unix epoch ms (UTC)      */
};

/* Schedule watchdog event types delivered to the user callback */
/* NOTE: enum/struct/typedef are defined in conexio_cloud.h — included above */

/* User-registered schedule callback (NULL if not registered) */
static conexio_schedule_cb_t g_schedule_cb = NULL;

/* k_timer used for the autonomous stop */
static struct k_timer  g_sched_wdt_timer;
static struct k_work   g_sched_wdt_work;

/* RAM copy of the active watchdog record (valid while timer is armed) */
static struct sched_watchdog_nvs g_sched_wdt;
static bool                      g_sched_wdt_active = false;

/* Forward declaration — dispatch_command is defined later in this file */
static void dispatch_command(const char *name, const char *payload_json);

/* Write the watchdog record to NVS and set the active flag */
static void sched_wdt_nvs_save(const struct sched_watchdog_nvs *rec)
{
#if defined(CONFIG_NVS)
    if (!nvs_initialised) return;
    uint8_t flag = 1U;
    nvs_write(&reboot_nvs, SCHED_WDT_ACTIVE_NVS_ID, &flag, sizeof(flag));
    nvs_write(&reboot_nvs, SCHED_WDT_RECORD_NVS_ID,  rec,  sizeof(*rec));
    LOG_DBG("Schedule watchdog: saved to NVS (stop=%s at %" PRId64 "ms)",
            rec->stop_command, rec->stop_at_ms);
#else
    ARG_UNUSED(rec);
    LOG_WRN("Schedule watchdog: NVS not enabled — stop command will not "
            "survive a reboot during the schedule window");
#endif
}

/* Clear the watchdog record from NVS */
static void sched_wdt_nvs_clear(void)
{
#if defined(CONFIG_NVS)
    if (!nvs_initialised) return;
    uint8_t flag = 0U;
    nvs_write(&reboot_nvs, SCHED_WDT_ACTIVE_NVS_ID, &flag, sizeof(flag));
    /* Overwrite record with zeros to remove stale data */
    struct sched_watchdog_nvs empty = {0};
    nvs_write(&reboot_nvs, SCHED_WDT_RECORD_NVS_ID, &empty, sizeof(empty));
    LOG_DBG("Schedule watchdog: cleared from NVS");
#endif
}

/* Work handler — runs in system work queue, fires when k_timer expires */
static void sched_wdt_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (!g_sched_wdt_active) return;
    g_sched_wdt_active = false;

    LOG_INF("Schedule watchdog: firing stop command '%s' (timer expired)",
            g_sched_wdt.stop_command);

    /* Execute the stop command locally */
    dispatch_command(g_sched_wdt.stop_command,
                     g_sched_wdt.stop_payload[0] != '\0'
                         ? g_sched_wdt.stop_payload : NULL);

    /* Clear NVS — schedule window complete */
    sched_wdt_nvs_clear();

    /* Notify user callback */
    if (g_schedule_cb) {
        struct conexio_schedule_event evt = {
            .type         = CONEXIO_SCHEDULE_EVT_STOPPED,
            .stop_command = g_sched_wdt.stop_command,
        };
        g_schedule_cb(&evt);
    }

    /* Zero the RAM record */
    memset(&g_sched_wdt, 0, sizeof(g_sched_wdt));
}

/* k_timer expiry function — submits work to the system work queue
 * (dispatch_command must not run directly from ISR/timer context) */
static void sched_wdt_timer_expiry(struct k_timer *timer)
{
    ARG_UNUSED(timer);
    k_work_submit(&g_sched_wdt_work);
}

/* Arm the schedule watchdog.
 * Called from transport_on_message() when a start command with stopAt arrives.
 * Also called from sched_watchdog_boot_check() to re-arm after reboot.
 *
 * stop_at_ms — Unix epoch milliseconds (UTC) when the stop command should fire.
 * stop_command / stop_payload — the command to dispatch when the timer fires.
 */
static void sched_wdt_arm(const char *stop_command, const char *stop_payload,
                           int64_t stop_at_ms)
{
    /* Cancel any previously armed timer */
    k_timer_stop(&g_sched_wdt_timer);

    /* Populate RAM record */
    strncpy(g_sched_wdt.stop_command, stop_command,   SCHED_WDT_CMD_MAX - 1);
    g_sched_wdt.stop_command[SCHED_WDT_CMD_MAX - 1] = '\0';
    strncpy(g_sched_wdt.stop_payload,
            stop_payload ? stop_payload : "{}",
            SCHED_WDT_PLD_MAX - 1);
    g_sched_wdt.stop_payload[SCHED_WDT_PLD_MAX - 1] = '\0';
    g_sched_wdt.stop_at_ms = stop_at_ms;
    g_sched_wdt_active     = true;

    /* Persist to NVS */
    sched_wdt_nvs_save(&g_sched_wdt);

    /* Compute delay — clamp to 1 ms minimum */
    int64_t now_ms = 0;
    date_time_now(&now_ms);
    int64_t delay_ms = stop_at_ms - now_ms;
    if (delay_ms < 1) delay_ms = 1;

    k_timer_start(&g_sched_wdt_timer, K_MSEC(delay_ms), K_NO_WAIT);

    LOG_INF("Schedule watchdog armed: '%s' fires in %" PRId64 " ms",
            stop_command, delay_ms);
}

/*
 * sched_watchdog_boot_check — called once from conexio_cloud_init().
 *
 * Checks NVS for a pending watchdog record from a previous session.
 * Three cases:
 *   a) No record (flag=0 or key missing) → nothing to do.
 *   b) Record found, stopAt still in future → re-arm the timer.
 *   c) Record found, stopAt already past → run stop command immediately
 *      (device was offline/powered-off during the schedule window).
 */
static void sched_watchdog_boot_check(void)
{
#if defined(CONFIG_NVS)
    if (!nvs_initialised) return;

    uint8_t flag = 0U;
    if (nvs_read(&reboot_nvs, SCHED_WDT_ACTIVE_NVS_ID,
                 &flag, sizeof(flag)) < 0 || flag == 0U) {
        LOG_DBG("Schedule watchdog: no pending record");
        return;
    }

    struct sched_watchdog_nvs rec = {0};
    if (nvs_read(&reboot_nvs, SCHED_WDT_RECORD_NVS_ID,
                 &rec, sizeof(rec)) < 0) {
        LOG_WRN("Schedule watchdog: active flag set but record unreadable — clearing");
        sched_wdt_nvs_clear();
        return;
    }

    if (rec.stop_command[0] == '\0') {
        LOG_WRN("Schedule watchdog: empty stop command in record — clearing");
        sched_wdt_nvs_clear();
        return;
    }

    int64_t now_ms = 0;
    bool clock_ok = (date_time_now(&now_ms) == 0);

    if (!clock_ok || rec.stop_at_ms > now_ms) {
        /* Clock not synced yet, or stop time still in future — re-arm.
         * If clock is not synced we re-arm with the full remaining window
         * approximated as (stopAt - 0) which overestimates; the timer
         * expiry simply runs the stop command when it fires. */
        int64_t delay_ms = clock_ok
            ? (rec.stop_at_ms - now_ms)
            : rec.stop_at_ms;                  /* fallback when no NTP     */
        if (delay_ms < 1) delay_ms = 1;

        strncpy(g_sched_wdt.stop_command, rec.stop_command, SCHED_WDT_CMD_MAX - 1);
        strncpy(g_sched_wdt.stop_payload, rec.stop_payload, SCHED_WDT_PLD_MAX - 1);
        g_sched_wdt.stop_at_ms = rec.stop_at_ms;
        g_sched_wdt_active     = true;

        k_timer_start(&g_sched_wdt_timer, K_MSEC(delay_ms), K_NO_WAIT);
        LOG_INF("Schedule watchdog: re-armed after reboot — '%s' fires in %" PRId64 " ms",
                rec.stop_command, delay_ms);
    } else {
        /* stopAt is in the past — execute stop command immediately */
        LOG_WRN("Schedule watchdog: stopAt already passed — running '%s' now",
                rec.stop_command);
        dispatch_command(rec.stop_command,
                         rec.stop_payload[0] != '\0' ? rec.stop_payload : NULL);
        sched_wdt_nvs_clear();

        if (g_schedule_cb) {
            /* NOTE: evt.stop_command points into stack-local rec.
             * This is safe because g_schedule_cb is called synchronously
             * here. Do not make this callback async without copying the
             * string to a persistent buffer first. */
            struct conexio_schedule_event evt = {
                .type         = CONEXIO_SCHEDULE_EVT_EXPIRED,
                .stop_command = rec.stop_command,
            };
            g_schedule_cb(&evt);
        }
    }
#else
    LOG_DBG("Schedule watchdog: NVS not enabled — boot check skipped");
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
    /* Priority order: most severe / actionable first.
     * Matches Memfault's nrfx_pmu_reboot_tracking.c decode order. */
    if (cause & RESET_WATCHDOG)       return "watchdog";
    if (cause & RESET_CPU_LOCKUP)     return "lockup";
    if (cause & RESET_BROWNOUT)       return "brownout";
    if (cause & RESET_SOFTWARE)       return "software";
    if (cause & RESET_PIN)            return "pin";
    if (cause & RESET_POR)            return "por";
    if (cause & RESET_LOW_POWER_WAKE) return "deepsleep"; /* GPIO/LPCOMP/VBUS wakeup */
    if (cause & RESET_DEBUG)          return "debug";     /* debugger-halted reset */
    /* cause == 0 means a clean Power-On Reset with no register bits set */
    if (cause == 0)                   return "por";
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
         * to avoid flooding the server after a long outage.
         * Payloads older than CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER_TTL_SEC are
         * discarded silently to prevent stale data polluting the cloud. */
        if (!offline_buffer_is_empty()) {
            int pending = offline_buffer_count();
            LOG_INF("Offline buffer: replaying %d payload(s)", pending);
            int replayed = 0;
            int discarded = 0;

#if CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER_TTL_SEC > 0
            /* Capture current time once for all TTL comparisons this session. */
            int64_t now_ms = 0;
            bool have_time = (date_time_now(&now_ms) == 0);
#endif

            while (!offline_buffer_is_empty() &&
                   replayed < CONFIG_CONEXIO_CLOUD_OFFLINE_REPLAY_BATCH) {
                char buf[OFFLINE_BUFFER_ENTRY_MAX];
                size_t buf_len;
                if (offline_buffer_peek(buf, &buf_len) != 0) break;

#if CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER_TTL_SEC > 0
                /* ── TTL check ─────────────────────────────────────────────
                 * Parse the "timestamp" (v2 SDK) or "ts" (v1 SDK) field from
                 * the buffered JSON.  If the payload is older than the TTL,
                 * pop it without publishing so stale data never reaches the
                 * cloud — especially important after long offline periods or
                 * when the device had a misconfigured short publish interval. */
                if (have_time) {
                    cJSON *entry = cJSON_ParseWithLength(buf, buf_len);
                    if (entry) {
                        const cJSON *ts_item = cJSON_GetObjectItemCaseSensitive(entry, "timestamp");
                        if (!ts_item) {
                            ts_item = cJSON_GetObjectItemCaseSensitive(entry, "ts");
                        }
                        if (cJSON_IsString(ts_item) && ts_item->valuestring) {
                            /* Parse ISO-8601 "YYYY-MM-DDTHH:MM:SS.mmmZ" */
                            struct tm t = {0};
                            int yr, mo, dy, hr, mi, sc;
                            if (sscanf(ts_item->valuestring,
                                       "%4d-%2d-%2dT%2d:%2d:%2d",
                                       &yr, &mo, &dy, &hr, &mi, &sc) == 6) {
                                t.tm_year = yr - 1900;
                                t.tm_mon  = mo - 1;
                                t.tm_mday = dy;
                                t.tm_hour = hr;
                                t.tm_min  = mi;
                                t.tm_sec  = sc;
                                int64_t payload_epoch = utc_tm_to_epoch(&t);
                                int64_t now_epoch     = now_ms / 1000LL;
                                int64_t age_sec       = now_epoch - payload_epoch;
                                if (age_sec > CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER_TTL_SEC) {
                                    LOG_DBG("Offline replay: discarding payload aged %llds "
                                            "(TTL=%ds)", (long long)age_sec,
                                            CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER_TTL_SEC);
                                    cJSON_Delete(entry);
                                    offline_buffer_pop();
                                    discarded++;
                                    continue;
                                }
                            }
                        }
                        cJSON_Delete(entry);
                    }
                }
#endif /* OFFLINE_BUFFER_TTL_SEC > 0 */

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
            LOG_INF("Offline replay: sent %d, discarded %d (TTL), %d remaining",
                    replayed, discarded, offline_buffer_count());
        }
#endif /* CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER */

#if defined(CONFIG_CONEXIO_CLOUD_FOTA)
        /* Check for any pending AWS IoT Jobs immediately on connect */
        fota_check_and_execute();
#endif
        break;

    case CONEXIO_CLOUD_EVT_DISCONNECTED:
        LOG_DBG("Cloud disconnected (intentional=%d)", (int)g_intentional_disconnect);
#if defined(CONFIG_CONEXIO_CLOUD_RETRY)
        if (!g_intentional_disconnect) {
            /* Unexpected disconnect — apply exponential backoff. */
            retry_on_failure();
        }
        /* Always clear the flag after use. */
        g_intentional_disconnect = false;
#endif
        break;

    case CONEXIO_CLOUD_EVT_PUBLISHED:
        LOG_DBG("Telemetry published");
#if defined(CONFIG_CONEXIO_CLOUD_PSM)
        /*
         * Disconnect MQTT cleanly before the modem enters PSM sleep.
         *
         * Rationale: MQTT keepalive is CONFIG_CONEXIO_CLOUD_MQTT_KEEPALIVE_SEC
         * (default 120s). The broker (AWS IoT Core) will force-disconnect the
         * client if no PINGREQ arrives within 1.5 × keepalive = ~180s.
         * With a PSM TAU of 3600s, the broker disconnects ~3 min into sleep
         * anyway — so we disconnect cleanly now instead:
         *  - Saves the broker from keeping a stale session open
         *  - Avoids any race where the transport thread sends a late PINGREQ
         *    just as the modem is entering sleep (brief spurious radio wakeup)
         *  - Clean disconnect + reconnect on wake is deterministic
         *
         * Note: transport_on_disconnected() will fire and call retry_on_failure().
         * retry_on_failure() must NOT trigger a reconnect here — we are
         * intentionally sleeping. The retry module's failure counter is reset
         * after a successful publish, so this is a fresh cycle.
         */
        if (transport_is_connected()) {
            LOG_DBG("Disconnecting MQTT before PSM sleep");
#if defined(CONFIG_CONEXIO_CLOUD_RETRY)
            /* Signal to the DISCONNECTED handler that this is intentional
             * so retry_on_failure() is skipped — no backoff, no reconnect. */
            g_intentional_disconnect = true;
            retry_on_success();  /* reset counter so wake-reconnect starts clean */
#endif
            transport_disconnect();
        }
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

static void builtin_on_set_interval(const char *payload_json, void *arg)
{
    ARG_UNUSED(arg);
    if (!payload_json) return;
    cJSON *p = cJSON_Parse(payload_json);
    if (!p) return;
    const cJSON *iv = cJSON_GetObjectItem(p, "interval");
    if (cJSON_IsNumber(iv)) {
        int new_sec = (int)iv->valuedouble;
        if (new_sec >= g_interval_min_sec && new_sec <= g_interval_max_sec) {
            g_sdk_interval_sec = new_sec;
            LOG_INF("SET_INTERVAL: publish interval → %ds", new_sec);
#if defined(CONFIG_NVS)
            /* Persist so the interval survives reboots. */
            if (nvs_initialised) {
                int32_t stored = (int32_t)new_sec;
                nvs_write(&reboot_nvs, INTERVAL_OVERRIDE_NVS_ID,
                          &stored, sizeof(stored));
                LOG_DBG("SET_INTERVAL: persisted %ds to NVS key 0x%04x",
                        new_sec, INTERVAL_OVERRIDE_NVS_ID);
            }
#endif
        } else {
            LOG_WRN("SET_INTERVAL: %d out of range [%d, %d] — ignoring",
                    new_sec, g_interval_min_sec, g_interval_max_sec);
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
    /* Validate against the application-registered limits (set via
     * conexio_cloud_register_interval). This ensures telemetryIntervalSec
     * from the OTA Config page honours the same bounds as SET_INTERVAL. */
    if (value < g_interval_min_sec || value > g_interval_max_sec) {
        LOG_WRN("telemetryIntervalSec: %d out of range [%d, %d] — ignoring",
                (int)value, g_interval_min_sec, g_interval_max_sec);
        return CONEXIO_SETTING_VALUE_OUT_OF_RANGE;
    }
    g_sdk_interval_sec = (int)value;
    LOG_INF("SDK: telemetryIntervalSec → %ds", g_sdk_interval_sec);
#if defined(CONFIG_NVS)
    /* Persist so the interval survives reboots — same NVS key as SET_INTERVAL. */
    if (nvs_initialised) {
        int32_t stored = (int32_t)value;
        nvs_write(&reboot_nvs, INTERVAL_OVERRIDE_NVS_ID, &stored, sizeof(stored));
        LOG_DBG("telemetryIntervalSec: persisted %ds to NVS key 0x%04x",
                (int)value, INTERVAL_OVERRIDE_NVS_ID);
    }
#endif
    return CONEXIO_SETTING_OK;
}

/* Default FOTA event handler — just logs progress.
 * Applications can supply their own via conexio_cloud_set_fota_cb(). */
#if defined(CONFIG_CONEXIO_CLOUD_FOTA)
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
            cJSON_free(doc_str);
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
/* dispatch_setting — dispatch a single key/value from a config push.
 * Returns true if the setting was applied successfully (CONEXIO_SETTING_OK),
 * false if it was rejected or had no registered handler. */
static bool dispatch_setting(const char *key, const cJSON *value_item)
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
                int32_t v = (int32_t)value_item->valuedouble;
                /* SDK range check — only when registered with _with_range */
                if (s->has_range && (v < (int32_t)s->range_min ||
                                     v > (int32_t)s->range_max)) {
                    LOG_WRN("Setting '%s': %d out of range [%d, %d] — rejected",
                            key, v,
                            (int32_t)s->range_min, (int32_t)s->range_max);
                    st = CONEXIO_SETTING_VALUE_OUT_OF_RANGE;
                } else {
                    st = s->cb_int(v, s->arg);
                }
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
                float v = (float)value_item->valuedouble;
                /* SDK range check — only when registered with _with_range */
                if (s->has_range && (v < (float)s->range_min ||
                                     v > (float)s->range_max)) {
                    LOG_WRN("Setting '%s': %.4g out of range [%.4g, %.4g] — rejected",
                            key, (double)v, s->range_min, s->range_max);
                    st = CONEXIO_SETTING_VALUE_OUT_OF_RANGE;
                } else {
                    st = s->cb_float(v, s->arg);
                }
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
        return (st == CONEXIO_SETTING_OK);
    }

    /* Key not in registry — safe to ignore; could be a newer config key
     * from the dashboard that this firmware version doesn't support yet.
     * Not counted as a failure — unknown keys are silently accepted. */
    LOG_DBG("Setting '%s' has no registered handler — ignoring", key);
    return true;
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
        const char *name       = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "command"));
        const char *command_id = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "commandId"));
        const char *valid_until= cJSON_GetStringValue(cJSON_GetObjectItem(msg, "validUntil"));

        if (name) {
            /* ── #7 Command deduplication ─────────────────────────────────
             *
             * Stores the last CMD_DEDUP_SLOTS commandIds seen this session.
             * Protects against QoS-1 redelivery after reconnect delivering
             * the same message twice in quick succession.
             *
             * Uses a simple fixed-size ring buffer of 8 slots — enough for
             * any realistic burst of commands without heap allocation.
             * Resets on reboot (RAM only, not NVS-persisted intentionally:
             * a reboot is a clean state and old IDs should be re-accepted).
             */
#define CMD_DEDUP_SLOTS 8
#define CMD_ID_MAX_LEN  64
            static char s_seen_ids[CMD_DEDUP_SLOTS][CMD_ID_MAX_LEN];
            static int  s_seen_head = 0;
            static bool s_dedup_init = false;
            if (!s_dedup_init) {
                memset(s_seen_ids, 0, sizeof(s_seen_ids));
                s_dedup_init = true;
            }
            bool is_duplicate = false;
            if (command_id && command_id[0] != '\0') {
                for (int i = 0; i < CMD_DEDUP_SLOTS; i++) {
                    if (s_seen_ids[i][0] != '\0' &&
                        strncmp(s_seen_ids[i], command_id, CMD_ID_MAX_LEN - 1) == 0) {
                        is_duplicate = true;
                        break;
                    }
                }
                if (!is_duplicate) {
                    /* Record this commandId in the ring */
                    strncpy(s_seen_ids[s_seen_head], command_id, CMD_ID_MAX_LEN - 1);
                    s_seen_ids[s_seen_head][CMD_ID_MAX_LEN - 1] = '\0';
                    s_seen_head = (s_seen_head + 1) % CMD_DEDUP_SLOTS;
                }
            }

            if (is_duplicate) {
                LOG_INF("Command '%s' (id=%s) already executed — skipping duplicate",
                        name, command_id ? command_id : "?");
                cJSON_Delete(msg);
                return;
            }

            /* ── #3 validUntil staleness check ───────────────────────────
             *
             * The scheduler executor sets validUntil = schedule.endAt for
             * start commands so a device waking after the window closes
             * does not execute a stale command (e.g. LED_ON after LED_OFF
             * window has already passed).
             *
             * We only skip if NTP is synced (date_time_now succeeds).
             * If the clock is not synced, we execute conservatively —
             * better to run a slightly stale command than to silently drop.
             *
             * End commands omit validUntil and are always executed.
             */
            if (valid_until && valid_until[0] != '\0') {
                int64_t now_ms = 0;
                if (date_time_now(&now_ms) == 0) {
                    /* Parse validUntil ISO-8601 string to ms since epoch.
                     * strptime is not available in Zephyr so we use a
                     * lightweight sscanf approach. */
                    /* Parse validUntil ISO-8601 UTC → epoch ms.
                     * Do NOT use mktime() — it interprets struct tm as local
                     * time. Zephyr date_time_now() is UTC, so we must compute
                     * epoch from UTC fields directly. */
                    struct tm tm_until = {0};
                    int yr, mo, dy, hr, mn, sc;
                    if (sscanf(valid_until, "%d-%d-%dT%d:%d:%d",
                               &yr, &mo, &dy, &hr, &mn, &sc) == 6) {
                        tm_until.tm_year = yr - 1900;
                        tm_until.tm_mon  = mo - 1;
                        tm_until.tm_mday = dy;
                        tm_until.tm_hour = hr;
                        tm_until.tm_min  = mn;
                        tm_until.tm_sec  = sc;
                        /* timegm converts UTC struct tm → epoch (POSIX extension).
                         * Zephyr's newlib provides timegm when POSIX_CLOCK is y. */
                        int64_t until_ms = (int64_t)utc_tm_to_epoch(&tm_until) * 1000LL;
                        if (now_ms > until_ms) {
                            LOG_WRN("Command '%s' expired (validUntil=%s) — skipping",
                                    name, valid_until);
                            cJSON_Delete(msg);
                            return;
                        }
                    }
                }
            }

            /* ── Payload extraction (unchanged) ──────────────────────── */
            const cJSON *payload_item = cJSON_GetObjectItem(msg, "payload");
            char *payload_json = NULL;
            if (cJSON_IsString(payload_item)) {
                /* Raw string value — already valid JSON text */
                const char *raw = cJSON_GetStringValue(payload_item);
                if (raw && strlen(raw) > 0) {
                    payload_json = k_malloc(strlen(raw) + 1);
                    if (payload_json) {
                        strcpy(payload_json, raw);
                    }
                }
            } else if (payload_item) {
                /* Object or array — serialize to compact JSON string */
                payload_json = cJSON_PrintUnformatted(payload_item);
            }
            dispatch_command(name, payload_json);
            LOG_DBG("Command dispatch: '%s' id=%s payload='%s'",
                    name,
                    command_id ? command_id : "none",
                    payload_json ? payload_json : "(none)");

            /* ── Schedule watchdog: arm on start command ─────────────────
             *
             * If the cloud included stopCommand + stopAt in this message
             * (set by executor.ts on start commands that have a run window),
             * arm the firmware-side watchdog timer so the stop command runs
             * autonomously even if the device loses cloud connectivity.
             *
             * stopAt is ISO-8601 UTC — parsed to epoch ms using sscanf.
             * We only arm if NTP is synced so the comparison is accurate.
             * If NTP is not synced we still arm with the absolute epoch
             * value; the timer may fire late but the stop will run.
             */
            const char *stop_cmd = cJSON_GetStringValue(
                cJSON_GetObjectItem(msg, "stopCommand"));
            const char *stop_at_str = cJSON_GetStringValue(
                cJSON_GetObjectItem(msg, "stopAt"));

            if (stop_cmd && stop_cmd[0] != '\0' &&
                stop_at_str && stop_at_str[0] != '\0') {
                /* Parse stopAt ISO-8601 → epoch ms */
                int yr2, mo2, dy2, hr2, mn2, sc2;
                if (sscanf(stop_at_str, "%d-%d-%dT%d:%d:%d",
                           &yr2, &mo2, &dy2, &hr2, &mn2, &sc2) == 6) {
                    struct tm tm2 = {0};
                    tm2.tm_year = yr2 - 1900;
                    tm2.tm_mon  = mo2 - 1;
                    tm2.tm_mday = dy2;
                    tm2.tm_hour = hr2;
                    tm2.tm_min  = mn2;
                    tm2.tm_sec  = sc2;
                    /* Use utc_tm_to_epoch (UTC) not mktime (local time) */
                    int64_t stop_at_ms = (int64_t)utc_tm_to_epoch(&tm2) * 1000LL;

                    /* Extract optional stopPayload */
                    const cJSON *spld = cJSON_GetObjectItem(msg, "stopPayload");
                    char *spld_json = NULL;
                    if (spld && !cJSON_IsNull(spld)) {
                        spld_json = cJSON_PrintUnformatted(spld);
                    }

                    sched_wdt_arm(stop_cmd, spld_json, stop_at_ms);

                    if (spld_json) {
                        cJSON_free(spld_json);
                    }

                    /* Notify user callback — schedule started */
                    if (g_schedule_cb) {
                        struct conexio_schedule_event evt = {
                            .type         = CONEXIO_SCHEDULE_EVT_STARTED,
                            .stop_command = stop_cmd,
                        };
                        g_schedule_cb(&evt);
                    }
                } else {
                    LOG_WRN("Schedule watchdog: could not parse stopAt '%s'",
                            stop_at_str);
                }
            }
            if (payload_json) {
                if (cJSON_IsString(payload_item)) {
                    k_free(payload_json);
                } else {
                    cJSON_free(payload_json);
                }
            }
        }

    } else if (strcmp(type, "config") == 0) {
        /* Iterate each key in the config object and dispatch individually.
         * This means a push with 5 settings fires 5 separate handler calls,
         * each with a typed value — no JSON parsing in application code.
         *
         * We aggregate the results: if any registered handler rejects its
         * value, success=false is sent in the config ACK so the dashboard
         * shows 'failed' instead of 'applied'. Unknown keys (no handler)
         * are treated as success so future dashboard keys don't break existing
         * firmware. */
        const cJSON *config_obj   = cJSON_GetObjectItem(msg, "config");
        const cJSON *version_item = cJSON_GetObjectItem(msg, "version");
        const char  *config_id    = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "configId"));
        uint32_t version = cJSON_IsNumber(version_item)
            ? (uint32_t)version_item->valuedouble : 0;

        LOG_INF("OTA Config push received (v%u)", version);

        bool all_ok = true;
        if (config_obj && cJSON_IsObject(config_obj)) {
            const cJSON *kv = NULL;
            cJSON_ArrayForEach(kv, config_obj) {
                if (!dispatch_setting(kv->string, kv)) {
                    all_ok = false;
                }
            }
        }

        /* Send config ACK with the real result — AFTER all handlers ran.
         * This replaces the early 'success: true' that was sent before
         * dispatch in the transport layer. */
        transport_config_ack(config_id, all_ok);
        if (!all_ok) {
            LOG_WRN("OTA Config v%u: one or more settings rejected — "
                    "dashboard will show 'failed'", version);
        } else {
            LOG_INF("OTA Config v%u applied — dashboard will show 'applied'", version);
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
    g_sdk_status = CONEXIO_CLOUD_STATUS_CONNECTED;
    k_sem_give(&g_connected_sem);  /* unblock conexio_cloud_wait_connected() */
    struct conexio_cloud_event evt = { .type = CONEXIO_CLOUD_EVT_CONNECTED };
    sdk_internal_event_handler(&evt);
}

void transport_on_disconnected(void)
{
    g_sdk_status = CONEXIO_CLOUD_STATUS_OFFLINE;
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
 * Returns a heap-allocated string — caller must cJSON_free() it.
 */
/* Forward declaration — build_payload_for_category defined below */
static char *build_payload_for_category(char category);

static char *build_payload(void)
{
    /* Default: build the app telemetry payload (backwards compatibility).
     * The new split-publish path in conexio_cloud_publish() calls
     * build_payload_for_category() directly. This shim handles the
     * offline buffer replay path which passes raw JSON strings anyway. */
    return build_payload_for_category(TOPIC_CAT_TELEMETRY);
}

/* ── build_payload_for_category ───────────────────────────────────────────
 *
 * Builds a JSON payload containing only the metrics that belong to the
 * given category. Each category maps to a separate versioned MQTT topic:
 *
 *   TOPIC_CAT_TELEMETRY   → v1/devices/{id}/telemetry    (app sensors)
 *   TOPIC_CAT_DIAGNOSTICS → v1/devices/{id}/diagnostics  (SDK system metrics)
 *   TOPIC_CAT_LOCATION    → v1/devices/{id}/location     (_loc_* metrics)
 *   TOPIC_CAT_LOGS        → v1/devices/{id}/logs         (_log stream)
 *
 * All payloads share the same envelope: deviceId, timestamp, seq, metrics.
 * The sequence number is per-topic so each stream can be monitored for gaps
 * independently.
 *
 * Returns a heap-allocated JSON string. Caller must cJSON_free() it.
 * Returns NULL on out-of-memory.
 */
static char *build_payload_for_category(char category)
{
    /* ── Timestamp ────────────────────────────────────────────────────── */
    char timestamp[40];
    int64_t unix_ms;

    if (date_time_now(&unix_ms) == 0) {
        /* NTP has synced — build a proper ISO-8601 UTC timestamp */
        time_t t = (time_t)(unix_ms / 1000);
        struct tm tm_buf;
        struct tm *tm_val = gmtime_r(&t, &tm_buf);
        int year = CLAMP(tm_val->tm_year + 1900, 2020, 2099);
        snprintf(timestamp, sizeof(timestamp),
                 "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                 year, tm_val->tm_mon + 1, tm_val->tm_mday,
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

    cJSON_AddStringToObject(root, "dev_id",    g_device_id);
    cJSON_AddStringToObject(root, "ts", timestamp);

    /* Sequence number — monotonically increasing per topic stream.
     * Lets the cloud detect gaps and reorder out-of-order delivery. */
    uint32_t *seq_ptr;
    const char *topic_name;
    switch (category) {
    case TOPIC_CAT_DIAGNOSTICS: seq_ptr = &g_seq_diagnostics; topic_name = "diagnostics"; break;
    case TOPIC_CAT_LOCATION:    seq_ptr = &g_seq_location;    topic_name = "location";    break;
    case TOPIC_CAT_LOGS:        seq_ptr = &g_seq_logs;        topic_name = "logs";        break;
    case TOPIC_CAT_TELEMETRY:   /* fall-through */
    default:                    seq_ptr = &g_seq_telemetry;   topic_name = "telemetry";   break;
    }
    cJSON_AddNumberToObject(root, "seq",   (double)(*seq_ptr)++);
    cJSON_AddStringToObject(root,  "topic", topic_name);

    cJSON_AddItemToObject(root, "metrics", metrics);

    /* ── Auto-metrics from the modem ─────────────────────────────────────
     * Only included in the DIAGNOSTICS payload (v1/.../diagnostics).
     * Sensor data and application metrics go to their own topics.
     */
    if (category == TOPIC_CAT_DIAGNOSTICS) {
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
     * _rssi: RSRP index. Prefer the async-cached value (updated via %CESQ
     * push notification) which is always fresh. Fall back to the polled
     * value from modem_info_params_get() if no notification received yet.
     * Skip entirely if both are invalid (255 = not measured).
     * Cloud converts index to dBm: idx < 0 → idx-140, idx > 0 → idx-141.
     * _snr: SNR index. SNR_IDX_TO_DB(x) = x-24. 127 = unavailable. */
    {
        int rsrp_val;
        if (g_cached_rsrp_idx != RSRP_CACHE_INVALID) {
            /* Use the freshest async notification value */
            rsrp_val = (int)g_cached_rsrp_idx;
        } else {
            /* Fall back to the polled value from modem_info_params_get */
            rsrp_val = (int)cached_modem_param.network.rsrp.value;
        }
        if (rsrp_val != RSRP_NOT_KNOWN_IDX) {
            cJSON_AddNumberToObject(metrics, "_rssi", (double)rsrp_val);
        }
    }
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
                cJSON_AddStringToObject(metrics, "_op", operator_buf);
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
                cJSON_AddStringToObject(metrics, "_mfw", modem_fw_buf);
            }
            /* _sdk_version — also first-publish-only */
            cJSON_AddStringToObject(metrics, "_sdk", CONEXIO_SDK_VERSION);
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
                cJSON_AddStringToObject(metrics, "_mfw", modem_fw_buf);
            }
        }
        cJSON_AddStringToObject(metrics, "_sdk", CONEXIO_SDK_VERSION);
        /* _fw_ver — application firmware version from VERSION file.
         * e.g. "1.0.0" built from VERSION_MAJOR/MINOR/PATCHLEVEL in app/VERSION.
         * Distinct from _sdk_version (Conexio SDK library version).
         * Tracked by the cloud and displayed in Fleet Health → Device Identity. */
        cJSON_AddStringToObject(metrics, "_fw_ver", CONEXIO_APP_FW_VERSION);
        /* _session_id — random 32-bit hex, unique per power-on session.
         * Lets the cloud correlate all packets from one boot across MQTT
         * reconnects without a flash write. Generated in conexio_cloud_init(). */
        if (g_session_id[0] != '\0') {
            cJSON_AddStringToObject(metrics, "_session_id", g_session_id);
        }
    }

    /* _operator: boot-once but retried until modem has attached */
    {
        static bool s_operator_sent = false;
        if (!s_operator_sent) {
            char operator_buf[MODEM_INFO_SHORT_OP_NAME_SIZE] = {0};
            if (modem_info_get_operator(operator_buf, sizeof(operator_buf)) == 0
                && operator_buf[0] != '\0') {
                cJSON_AddStringToObject(metrics, "_op", operator_buf);
                s_operator_sent = true;
            }
        }
    }

    /* ── SLOW / delta metrics ─────────────────────────────────────────────
     *
     * _lte_band, _cell_id, _tac change only on cell handover or band
     * reselection — rare events for a stationary device. Sending them on
     * every publish wastes data for no benefit.
     *
     * Strategy (delta encoding):
     *   - Always send on boot (emit_boot) so the cloud has the initial value.
     *   - Send on every slow_interval tick as a heartbeat (cloud confirmation).
     *   - Also send immediately when the value changes, regardless of the timer,
     *     so cell handovers are captured in real time.
     *
     * Previous values are stored in persistent statics. On first call they are
     * set to SENTINEL values so the first publish always triggers a send.
     */
    {
        static uint8_t  s_prev_band    = 0xFF;         /* BAND_UNAVAILABLE sentinel */
        static uint32_t s_prev_cell_id = 0xFFFFFFFF;   /* LTE_LC_CELL_EUTRAN_ID_INVALID */
        static uint16_t s_prev_tac     = 0xFFFF;       /* TAC invalid sentinel */

        const struct conexio_lte_session_metrics *lm_slow =
            conexio_lte_get_session_metrics();

        /* _lte_band */
        {
            uint8_t band = 0;
            if (modem_info_get_current_band(&band) == 0 && band != BAND_UNAVAILABLE) {
                bool changed = (band != s_prev_band);
                if (emit_boot || emit_slow || changed) {
                    cJSON_AddNumberToObject(metrics, "_lte_band", (double)band);
                    if (changed) {
                        LOG_INF("Delta: _lte_band %u → %u", s_prev_band, band);
                    }
                    s_prev_band = band;
                }
            }
        }

        /* _cell_id */
        if (lm_slow->cell_id != 0xFFFFFFFF) {
            bool changed = (lm_slow->cell_id != (uint32_t)s_prev_cell_id);
            if (emit_boot || emit_slow || changed) {
                cJSON_AddNumberToObject(metrics, "_cell_id",
                                        (double)lm_slow->cell_id);
                if (changed) {
                    LOG_INF("Delta: _cell_id %u → %u",
                            s_prev_cell_id, lm_slow->cell_id);
                }
                s_prev_cell_id = lm_slow->cell_id;
            }
        }

        /* _tac */
        if (lm_slow->tac != 0xFFFFFFFF) {
            uint16_t tac = (uint16_t)lm_slow->tac;
            bool changed = (tac != s_prev_tac);
            if (emit_boot || emit_slow || changed) {
                cJSON_AddNumberToObject(metrics, "_tac", (double)tac);
                if (changed) {
                    LOG_INF("Delta: _tac %u → %u", s_prev_tac, tac);
                }
                s_prev_tac = tac;
            }
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

    /* _publish_success_count / _publish_fail_count ─────────────────────
     * Accumulated since boot. Lets the cloud compute per-device publish
     * reliability: success_rate = success / (success + fail) × 100%.
     * Omitted on the very first publish (both are 0 — meaningless noise).
     * From the second publish onward the counter reflects real history. */
    if (g_publish_success_count > 0 || g_publish_fail_count > 0) {
        cJSON_AddNumberToObject(metrics, "_pub_ok",
                                (double)g_publish_success_count);
        if (g_publish_fail_count > 0) {
            cJSON_AddNumberToObject(metrics, "_publish_fail_count",
                                    (double)g_publish_fail_count);
        }
    }

#if defined(CONFIG_CONEXIO_CLOUD_BATTERY_METRICS)
    /* _battery_soc_pct / _battery_drain_pct_hr ──────────────────────────
     * Read state-of-charge from the nPM1300 fuel gauge.
     * Drain rate is computed as: (prev_soc - cur_soc) / elapsed_hours.
     * Only emitted during discharge — skipped while charging or if SOC
     * increased (e.g. charger plugged in between publishes). */
    {
        bool is_charging = false;
        float soc = battery_read_soc(&is_charging);
        if (soc >= 0.0f) {
            cJSON_AddNumberToObject(metrics, "_batt_soc", (double)soc);

            int64_t now_ms = k_uptime_get();
            if (!is_charging && g_last_soc_pct >= 0.0f && g_last_pub_time_ms > 0) {
                float elapsed_hr = (float)(now_ms - g_last_pub_time_ms) / 3600000.0f;
                if (elapsed_hr > 0.0f) {
                    float drain = (g_last_soc_pct - soc) / elapsed_hr;
                    /* Only emit when actually draining (positive drain rate).
                     * Negative values mean SOC went up — charger was removed
                     * and reconnected mid-interval, not a meaningful reading. */
                    if (drain > 0.0f) {
                        cJSON_AddNumberToObject(metrics, "_batt_drain_hr",
                                                (double)drain);
                    }
                }
            }
            /* Update tracking state for next publish */
            g_last_soc_pct     = soc;        /* float — preserves sub-percent precision */
            g_last_pub_time_ms = now_ms;
        }
    }
#endif /* CONFIG_CONEXIO_CLOUD_BATTERY_METRICS */

#if defined(CONFIG_CONEXIO_CLOUD_AUTO_BATTERY)
    /* Battery voltage via AT%XVBAT — uses modem_info_get_batt_voltage()
     * which calls nrf_modem_at_scanf("AT%%XVBAT", "%%XVBAT: %%d", &val)
     * directly.  More reliable than modem_info_string_get(MODEM_INFO_BATTERY)
     * + atoi() because it parses the integer in the AT response directly
     * rather than converting the number back through a string intermediate.
     * Matches the pattern used in the conexio-stratus-provision mqtt sample. */
    {
        int bat_mv = 0;
        if (modem_info_get_batt_voltage(&bat_mv) == 0 && bat_mv > 0) {
            cJSON_AddNumberToObject(metrics, "_batt_mv", (double)bat_mv);
        }
    }
#endif /* CONFIG_CONEXIO_CLOUD_AUTO_BATTERY */

    } /* end if (category == TOPIC_CAT_DIAGNOSTICS) */

    /* ── Registered sensor callbacks (TELEMETRY topic only) ──────────── */
    if (category == TOPIC_CAT_TELEMETRY) {
    for (int i = 0; i < sensor_count; i++) {
        if (!sensor_registry[i].used) continue;
        double val = sensor_registry[i].callback(sensor_registry[i].arg);
        if (!isnan(val)) {
            cJSON_AddNumberToObject(metrics, sensor_registry[i].name, val);
        }
    }
    } /* end TELEMETRY sensor callbacks */

    /* ── Application metrics from the queue ──────────────────────────── */
    k_mutex_lock(&queue_mutex, K_FOREVER);
    for (int i = 0; i < CONFIG_CONEXIO_CLOUD_METRIC_QUEUE_SIZE; i++) {
        if (!metric_queue[i].used) continue;

        /* Only include metrics that belong to this payload's category */
        if (metric_queue[i].category != category) continue;

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

#if defined(CONFIG_CONEXIO_CLOUD_LOG_STREAM)
    /* ── Cloud log stream ─────────────────────────────────────────────────
     * Only included in the LOGS payload (v1/.../logs).
     * The array is omitted entirely when the buffer is empty. */
    if (category == TOPIC_CAT_LOGS) {
        log_stream_drain(metrics);
    }
#endif

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
            /* Wait up to 120s — some NB-IoT networks take 60-120s to re-register.
             * 30s was too short and caused spurious "wake timeout" warnings. */
            if (power_mgr_wake(120) != 0) {
                LOG_WRN("PSM wake timeout (120s) — modem may be struggling to register");
                /* Still try to reconnect below; transport_connect() will handle
                 * the case where LTE is not yet ready. */
            }
            /* Modem just woke from PSM — drain any queued commands before
             * publishing. AWS may have queued messages during the sleep. */
            LOG_DBG("PSM wake: draining incoming messages...");
            for (int drain = 0; drain < 10; drain++) {
                transport_poll(K_MSEC(200));
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
            /* Drain the socket immediately after reconnect — AWS IoT Core
             * may have queued commands (QoS 1) while the device was offline
             * or in PSM sleep. Process them now before the next publish so
             * commands like REBOOT are handled without waiting for the next
             * poll cycle.
             * Poll for up to 2 seconds total in 200ms windows. */
            LOG_DBG("Post-reconnect: draining incoming messages...");
            for (int drain = 0; drain < 10; drain++) {
                transport_poll(K_MSEC(200));
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

                    /* Track publish reliability */
                    if (pub_ret == 0) {
                        g_publish_success_count++;
                    } else {
                        g_publish_fail_count++;
                    }

#if defined(CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER)
                    /* Buffer the payload if publish failed due to no connection */
                    if (pub_ret == -ENOTCONN) {
                        char *payload = build_payload();
                        if (payload) {
                            if (offline_buffer_push(payload, strlen(payload)) == 0) {
                                LOG_INF("Offline: buffered payload (%d pending)",
                                        offline_buffer_count());
                            }
                            cJSON_free(payload);
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
    setting_registry[setting_count].key       = key;
    setting_registry[setting_count].type      = SETTING_INT;
    setting_registry[setting_count].cb_int    = handler;
    setting_registry[setting_count].arg       = arg;
    setting_registry[setting_count].has_range = false;
    setting_count++;
    LOG_DBG("Setting registered (int): '%s'", key);
    return 0;
}

/* conexio_cloud_register_setting_int_with_range — int setting with SDK range check */
int conexio_cloud_register_setting_int_with_range(const char *key,
                                                  int32_t min, int32_t max,
                                                  conexio_int_setting_cb_t handler,
                                                  void *arg)
{
    if (!key || !handler || max < min) return -EINVAL;
    if (setting_count >= CONFIG_CONEXIO_CLOUD_MAX_SETTINGS) return -ENOMEM;
    setting_registry[setting_count].key       = key;
    setting_registry[setting_count].type      = SETTING_INT;
    setting_registry[setting_count].cb_int    = handler;
    setting_registry[setting_count].arg       = arg;
    setting_registry[setting_count].has_range = true;
    setting_registry[setting_count].range_min = (double)min;
    setting_registry[setting_count].range_max = (double)max;
    setting_count++;
    LOG_DBG("Setting registered (int, range [%d, %d]): '%s'", min, max, key);
    return 0;
}

/* conexio_cloud_register_setting_bool — register a boolean setting handler */
int conexio_cloud_register_setting_bool(const char *key,
                                        conexio_bool_setting_cb_t handler,
                                        void *arg)
{
    if (!key || !handler) return -EINVAL;
    if (setting_count >= CONFIG_CONEXIO_CLOUD_MAX_SETTINGS) return -ENOMEM;
    setting_registry[setting_count].key       = key;
    setting_registry[setting_count].type      = SETTING_BOOL;
    setting_registry[setting_count].cb_bool   = handler;
    setting_registry[setting_count].arg       = arg;
    setting_registry[setting_count].has_range = false;
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
    setting_registry[setting_count].key       = key;
    setting_registry[setting_count].type      = SETTING_FLOAT;
    setting_registry[setting_count].cb_float  = handler;
    setting_registry[setting_count].arg       = arg;
    setting_registry[setting_count].has_range = false;
    setting_count++;
    LOG_DBG("Setting registered (float): '%s'", key);
    return 0;
}

/* conexio_cloud_register_setting_float_with_range — float setting with SDK range check */
int conexio_cloud_register_setting_float_with_range(const char *key,
                                                    float min, float max,
                                                    conexio_float_setting_cb_t handler,
                                                    void *arg)
{
    if (!key || !handler || max < min) return -EINVAL;
    if (setting_count >= CONFIG_CONEXIO_CLOUD_MAX_SETTINGS) return -ENOMEM;
    setting_registry[setting_count].key       = key;
    setting_registry[setting_count].type      = SETTING_FLOAT;
    setting_registry[setting_count].cb_float  = handler;
    setting_registry[setting_count].arg       = arg;
    setting_registry[setting_count].has_range = true;
    setting_registry[setting_count].range_min = (double)min;
    setting_registry[setting_count].range_max = (double)max;
    setting_count++;
    LOG_DBG("Setting registered (float, range [%.4g, %.4g]): '%s'",
            (double)min, (double)max, key);
    return 0;
}

/* conexio_cloud_register_setting_string — register a string setting handler */
int conexio_cloud_register_setting_string(const char *key,
                                          conexio_string_setting_cb_t handler,
                                          void *arg)
{
    if (!key || !handler) return -EINVAL;
    if (setting_count >= CONFIG_CONEXIO_CLOUD_MAX_SETTINGS) return -ENOMEM;
    setting_registry[setting_count].key        = key;
    setting_registry[setting_count].type       = SETTING_STRING;
    setting_registry[setting_count].cb_string  = handler;
    setting_registry[setting_count].arg        = arg;
    setting_registry[setting_count].has_range  = false; /* no range for strings */
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

    /* ── Step 0: Initialise the nRF Modem Library ───────────────────────
     * Must happen before any modem AT command or LTE call.
     * nrf_modem_lib_init() is idempotent — safe to call if already done. */
    ret = nrf_modem_lib_init();
    if (ret != 0 && ret != -EALREADY) {
        LOG_ERR("nrf_modem_lib_init() failed (%d)", ret);
        return ret;
    }

    /* ── Step 1: Reboot counter ─────────────────────────────────────── */
    reboot_counter_init();

    /* ── Step 1b: Reboot reason ─────────────────────────────────────── */
    /* Must be called AFTER reboot_counter_init() because reboot_reason_init()
     * may write to NVS, which requires nvs_initialised = true. */
    reboot_reason_init();

    /* ── Step 1c: Schedule watchdog init ────────────────────────────── */
    /* Init once — idempotent (called only inside conexio_cloud_init). */
    k_timer_init(&g_sched_wdt_timer, sched_wdt_timer_expiry, NULL);
    k_work_init(&g_sched_wdt_work, sched_wdt_work_handler);

    /* ── Step 2: Initialise modem info ──────────────────────────────────
     * modem_info_init() registers the AT notification handlers.
     * Must happen before any modem_info_* call. */
    modem_info_init();
    if (modem_info_connectivity_stats_init() != 0) {
        LOG_WRN("modem_info_connectivity_stats_init failed — _tx_kb/_rx_kb unavailable");
    }

    /* Subscribe to %CESQ push notifications so g_cached_rsrp_idx is always
     * fresh. The modem sends %CESQ every time it measures signal quality —
     * typically every 5-10s while active. Using the cached value at publish
     * time avoids the race where AT+CESQ (polled in modem_info_params_get)
     * returns 255 right after PSM wake before the first measurement. */
    modem_info_rsrp_register(on_rsrp_notification);

#if defined(CONFIG_CONEXIO_CLOUD_BATTERY_METRICS)
    battery_metrics_init();
#endif

    /* ── Step 2b: Derive device ID (IMEI) ───────────────────────────────
     * modem_info_string_get(MODEM_INFO_IMEI) issues a single AT+CGSN command.
     * This works in offline/AT-command mode — no LTE registration needed.
     * (modem_info_params_get() is different: it issues multiple AT commands
     *  for ALL modem info categories and requires normal functional mode /
     *  network registration — that is why it failed after nrf_modem_lib_init()
     *  but before lte_lc_connect_async().)
     *
     * The provisioning sample (conexio-stratus-provision/src/main.c) uses
     * this same function before LTE connects — this is the correct approach. */
#if defined(CONFIG_CONEXIO_CLOUD_STATIC_DEVICE_ID_ENABLED)
    strncpy(g_device_id, CONFIG_CONEXIO_CLOUD_STATIC_DEVICE_ID,
            sizeof(g_device_id) - 1);
    g_device_id[sizeof(g_device_id) - 1] = '\0';
    LOG_INF("Device ID (static override): %s", g_device_id);
#else
    {
        char imei[MODEM_INFO_MAX_RESPONSE_SIZE] = {0};
        int imei_ret = modem_info_string_get(MODEM_INFO_IMEI,
                                             imei, sizeof(imei));
        if (imei_ret < 0) {
            LOG_ERR("modem_info_string_get(IMEI) failed: %d", imei_ret);
            dispatch_error(imei_ret);
            return imei_ret;
        }
        /* Strip trailing CR / LF / space */
        for (int i = (int)strlen(imei) - 1; i >= 0; i--) {
            if (imei[i] == '\r' || imei[i] == '\n' || imei[i] == ' ') {
                imei[i] = '\0';
            } else {
                break;
            }
        }
        strncpy(g_device_id, imei, sizeof(g_device_id) - 1);
        g_device_id[sizeof(g_device_id) - 1] = '\0';
        LOG_INF("Device ID (IMEI): %s", g_device_id);
    }
#endif

    LOG_INF("Registered: %d command(s), %d setting(s)", cmd_count, setting_count);

    /* ── Step 3: Provision TLS credentials ─────────────────────────────
     * Must happen BEFORE LTE connects. write_if_absent() only writes when
     * credentials are absent, so it takes the modem offline only on the
     * first boot. On subsequent boots it's a no-op (certs already present).
     * Doing this now avoids dropping LTE mid-session. */
    {
        /* Use a dummy config — cert_store now uses embedded certs, not cfg */
        struct conexio_cloud_config_t dummy_cfg = {0};
        ret = cert_store_provision_from_config(&dummy_cfg);
        if (ret) {
            LOG_ERR("TLS credential provisioning failed (%d)", ret);
            dispatch_error(ret);
            return ret;
        }
    }

    /* ── Step 4: Retry + watchdog init ─────────────────────────────── */
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

    /* ── Step 6b: Schedule watchdog boot check ──────────────────────── */
    /* Run after NTP so the clock comparison in sched_watchdog_boot_check()
     * uses an accurate current time.  If NTP failed, the check still runs
     * conservatively (re-arms timer rather than silently dropping). */
    sched_watchdog_boot_check();
#endif

    /* ── Step 7: Fetch cloud config from Conexio config service ─────── */
    struct conexio_cloud_config_t cloud_cfg;
    ret = config_fetch(g_device_id, &cloud_cfg);
    if (ret) {
        LOG_ERR("config_fetch() failed (%d)", ret);
        dispatch_error(ret);
        return ret;
    }

    /* ── Step 8: Provision TLS credentials — already done in Step 3 ─── */
    /* cert_store_provision_from_config() was called before LTE connect   */
    /* to avoid dropping LTE mid-session. Nothing to do here.             */

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

        /* Wait for the network's PSM grant/deny decision before proceeding.
         *
         * The network sends LTE_LC_EVT_PSM_UPDATE within ~100-500 ms of
         * registration. Waiting here ensures:
         *   1. The first telemetry payload carries accurate _psm_tau_sec and
         *      _psm_active_sec values (not the -1 sentinel).
         *   2. The device knows whether PSM was granted before it connects to
         *      MQTT, so power_mgr_is_psm_active() is reliable from the start.
         *
         * 5s timeout is generous — if the network hasn't responded by then
         * it almost certainly won't grant PSM this session. We proceed either
         * way; a timeout is logged as a warning, not treated as an error. */
        power_mgr_wait_psm_decision(5);
    }
#endif

    /* ── Step 11: FOTA init ─────────────────────────────────────────── */
#if defined(CONFIG_CONEXIO_CLOUD_FOTA)
    fota_init(g_device_id, sdk_fota_event_handler);
    /* Post-FOTA boot: confirm the new image so MCUboot won't roll back,
     * and persist the completed job ID to NVS so SUCCEEDED is published
     * to AWS IoT Jobs on the first MQTT connect (dashboard update). */
    fota_confirm();
#endif

    /* ── Step 12: Spawn background thread ──────────────────────────── */
    g_sdk_status = CONEXIO_CLOUD_STATUS_CONNECTING;
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
#if defined(CONFIG_CONEXIO_CLOUD_METRIC_OVERFLOW_LOG)
        LOG_WRN("Metric queue full (size=%d) — '%s' dropped. "
                "Increase CONFIG_CONEXIO_CLOUD_METRIC_QUEUE_SIZE.",
                CONFIG_CONEXIO_CLOUD_METRIC_QUEUE_SIZE, name);
#endif
        return -ENOMEM;
    }
    strncpy(metric_queue[slot].name, name, MAX_METRIC_NAME - 1);
    metric_queue[slot].type    = 'n';
    metric_queue[slot].num_val = value;
    metric_queue[slot].used    = true;
    /* Auto-tag _loc_* metrics to the location topic, everything else to
     * telemetry. SDK internal metrics (_rssi, _reboot etc.) are added
     * directly in build_payload_for_category under the diagnostics guard
     * and never pass through this queue. */
    metric_queue[slot].category = (strncmp(name, "_loc_", 5) == 0)
                                  ? TOPIC_CAT_LOCATION
                                  : TOPIC_CAT_TELEMETRY;
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
    metric_queue[slot].type     = 's';
    metric_queue[slot].category = (strncmp(name, "_loc_", 5) == 0)
                                  ? TOPIC_CAT_LOCATION : TOPIC_CAT_TELEMETRY;
    metric_queue[slot].used     = true;
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
    metric_queue[slot].category = TOPIC_CAT_TELEMETRY;
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
/* ── Internal: publish a single category ─────────────────────────────────────
 * Used by cell_location.c to publish ONLY the location topic after a fix.
 * Avoids re-publishing diagnostics/telemetry which would duplicate boot-once
 * metrics and cause the dashboard to show multiple diagnostics on boot.
 */
int conexio_cloud_publish_single(char category)
{
    if (!transport_is_connected()) return -ENOTCONN;

    /* Check if there is anything to send for this category */
    bool has_data = false;
    if (category == TOPIC_CAT_DIAGNOSTICS) {
        has_data = true;
    } else if (category == TOPIC_CAT_LOGS) {
#if defined(CONFIG_CONEXIO_CLOUD_LOG_STREAM)
        has_data = (log_stream_pending() > 0);
#endif
    } else {
        if (category == TOPIC_CAT_TELEMETRY && sensor_count > 0) {
            has_data = true;
        }
        if (!has_data) {
            k_mutex_lock(&queue_mutex, K_FOREVER);
            for (int i = 0; i < CONFIG_CONEXIO_CLOUD_METRIC_QUEUE_SIZE; i++) {
                if (metric_queue[i].used && metric_queue[i].category == category) {
                    has_data = true;
                    break;
                }
            }
            k_mutex_unlock(&queue_mutex);
        }
    }
    if (!has_data) return 0;

    char *payload = build_payload_for_category(category);
    if (!payload) return -ENOMEM;
    int ret = transport_publish_to(category, payload, strlen(payload));
    cJSON_free(payload);
    return ret;
}

int conexio_cloud_publish(void)
{
    if (!transport_is_connected()) return -ENOTCONN;

    int overall = 0;

    /* Publish each category to its own versioned topic.
     * Skip categories that have nothing to send — build_payload_for_category
     * will still produce a valid envelope, but we avoid unnecessary radio
     * wakeups by checking for pending data first.
     *
     * Diagnostics: always publish (SDK auto-metrics fire every cycle).
     * Telemetry:   publish only if sensor callbacks are registered or
     *              app metrics are queued for this category.
     * Location:    publish only if _loc_* metrics are queued.
     * Logs:        publish only if log entries are pending.
     */
    static const char categories[] = {
        TOPIC_CAT_DIAGNOSTICS,
        TOPIC_CAT_TELEMETRY,
        TOPIC_CAT_LOCATION,
        TOPIC_CAT_LOGS,
    };

    for (int c = 0; c < (int)sizeof(categories); c++) {
        char cat = categories[c];

        /* Quick check: does this category have anything to send? */
        bool has_data = false;

        if (cat == TOPIC_CAT_DIAGNOSTICS) {
            has_data = true; /* Always — SDK auto-metrics */
        } else if (cat == TOPIC_CAT_LOGS) {
#if defined(CONFIG_CONEXIO_CLOUD_LOG_STREAM)
            has_data = (log_stream_pending() > 0);
#endif
        } else {
            /* TELEMETRY or LOCATION — check queue and sensor registry */
            if (cat == TOPIC_CAT_TELEMETRY && sensor_count > 0) {
                has_data = true;
            }
            if (!has_data) {
                k_mutex_lock(&queue_mutex, K_FOREVER);
                for (int i = 0; i < CONFIG_CONEXIO_CLOUD_METRIC_QUEUE_SIZE; i++) {
                    if (metric_queue[i].used && metric_queue[i].category == cat) {
                        has_data = true;
                        break;
                    }
                }
                k_mutex_unlock(&queue_mutex);
            }
        }

        if (!has_data) {
            continue;
        }

        char *payload = build_payload_for_category(cat);
        if (!payload) {
            LOG_ERR("build_payload_for_category(%c) returned NULL", cat);
            overall = -ENOMEM;
            continue;
        }

        int ret = transport_publish_to(cat, payload, strlen(payload));
        cJSON_free(payload);

        if (ret != 0) {
            LOG_WRN("Publish failed for category '%c' (%d)", cat, ret);
            overall = ret;
        }
    }

    if (overall == 0) {
        /* Poll briefly to allow PUBACK to arrive before firing EVT_PUBLISHED.
         * QoS 1 PUBACK from AWS IoT Core typically arrives in 50-200ms.
         * Without this poll, EVT_PUBLISHED fires immediately after mqtt_publish()
         * returns — before PUBACK — causing the pre-sleep disconnect to happen
         * while the broker is still waiting to deliver the PUBACK. */
        transport_poll(K_MSEC(500));
        struct conexio_cloud_event evt = { .type = CONEXIO_CLOUD_EVT_PUBLISHED };
        sdk_internal_event_handler(&evt);
    }

    return overall;
}

int conexio_cloud_publish_alert(const char *name, double value, double threshold)
{
    if (!name) return -EINVAL;
    if (!transport_is_connected()) return -ENOTCONN;

    /* Build a compact alert JSON envelope */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "dev_id",  g_device_id);

    char timestamp[32];   /* "2026-08-29T01:42:23.123Z" = 24 chars + null */
    int64_t unix_ms;
    if (date_time_now(&unix_ms) == 0) {
        time_t t = (time_t)(unix_ms / 1000);
        struct tm tm_buf;
        struct tm *tm_val = gmtime_r(&t, &tm_buf);
        /* Clamp all fields to their valid ranges so the compiler can prove
         * the snprintf output fits — suppresses -Wformat-truncation. */
        int year  = CLAMP(tm_val->tm_year + 1900, 2020, 2099); /* 4 digits */
        int mon   = CLAMP(tm_val->tm_mon  + 1,    1,    12);
        int day   = CLAMP(tm_val->tm_mday,         1,    31);
        int hour  = CLAMP(tm_val->tm_hour,         0,    23);
        int min   = CLAMP(tm_val->tm_min,          0,    59);
        int sec   = CLAMP(tm_val->tm_sec,          0,    60); /* 60 = leap second */
        unsigned ms = (unsigned)(llabs(unix_ms) % 1000U);     /* 0–999 */
        snprintf(timestamp, sizeof(timestamp),
                 "%04d-%02d-%02dT%02d:%02d:%02d.%03uZ",
                 year, mon, day, hour, min, sec, ms);
    } else {
        strncpy(timestamp, "1970-01-01T00:00:00.000Z", sizeof(timestamp));
    }
    cJSON_AddStringToObject(root, "ts", timestamp);
    cJSON_AddStringToObject(root, "metric",    name);
    cJSON_AddNumberToObject(root, "value",     value);
    cJSON_AddNumberToObject(root, "threshold", threshold);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return -ENOMEM;

    int ret = transport_publish_alert(json, strlen(json));
    cJSON_free(json);

    if (ret == 0) {
        LOG_INF("Alert published: %s=%.2f (threshold=%.2f)", name, value, threshold);
    }
    return ret;
}

/** Returns true if the transport is currently connected to the cloud. */
bool conexio_cloud_is_connected(void)  { return transport_is_connected(); }
const char *conexio_cloud_device_id(void) { return g_device_id; }

/** Returns the current publish interval in seconds (may differ from Kconfig
 *  default if SET_INTERVAL or telemetryIntervalSec OTA Config was applied). */
int conexio_cloud_get_interval_sec(void) { return g_sdk_interval_sec; }

/**
 * conexio_cloud_register_interval — register interval limits for SET_INTERVAL.
 *
 * Must be called before conexio_cloud_init().
 * min_sec clamped to >= 1, max_sec clamped to >= min_sec.
 */
void conexio_cloud_register_interval(int min_sec, int max_sec)
{
    g_interval_min_sec = MAX(1, min_sec);
    g_interval_max_sec = MAX(g_interval_min_sec, max_sec);
    LOG_INF("Interval registered: [%ds, %ds]",
            g_interval_min_sec, g_interval_max_sec);
}

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

/**
 * conexio_cloud_register_schedule_cb — register a schedule lifecycle callback.
 *
 * Called by the SDK when:
 *   CONEXIO_SCHEDULE_EVT_STARTED — start command received, watchdog armed.
 *   CONEXIO_SCHEDULE_EVT_STOPPED — stop command fired autonomously (timer).
 *   CONEXIO_SCHEDULE_EVT_EXPIRED — stopAt was in the past on boot; ran now.
 *
 * Must be called before conexio_cloud_init().
 */
void conexio_cloud_register_schedule_cb(conexio_schedule_cb_t cb)
{
    g_schedule_cb = cb;
    LOG_DBG("Schedule callback registered");
}

/**
 * conexio_cloud_cancel_schedule_watchdog — disarm the schedule watchdog.
 *
 * Call if the stop command arrives from the cloud before the timer fires
 * (i.e. the cloud sent LED_OFF and it was received successfully).
 * This prevents the watchdog from firing a duplicate stop command.
 */
void conexio_cloud_cancel_schedule_watchdog(void)
{
    if (!g_sched_wdt_active) return;
    k_timer_stop(&g_sched_wdt_timer);
    g_sched_wdt_active = false;
    sched_wdt_nvs_clear();
    memset(&g_sched_wdt, 0, sizeof(g_sched_wdt));
    LOG_INF("Schedule watchdog cancelled (stop received from cloud)");
}
