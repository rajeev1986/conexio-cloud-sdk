/*
 * main.c — Conexio Console MQTT telemetry sample
 *
 * nRF Connect SDK v3.2.1 / nRF91xx
 *
 * Telemetry metrics (ported from conexio_cloud.c build_payload):
 *
 *  BOOT-ONCE (first publish after each reboot):
 *    _reboot_cnt, _reboot_reason, _sdk_version, _modem_fw,
 *    _lte_mode, _lte_connect_ms,
 *    _psm_tau_sec, _psm_active_sec, _edrx_ms
 *
 *  SLOW (every 10 publishes ≈ 10 min at 60s interval):
 *    _lte_band, _cell_id, _tac
 *
 *  EVERY publish:
 *    _rssi, _snr, _conn_loss, _reset_loop, _tx_kb, _rx_kb,
 *    _modem_temp, _battery_mv
 *
 *  Application sensors (every publish):
 *    temperature, humidity
 */

#include <zephyr/kernel.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/random/random.h>
#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include <modem/modem_info.h>
#include <date_time.h>
#include <cJSON.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "aws_certs.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

/* ── Configuration ───────────────────────────────────────────────────────── */
#define DEVICE_ID        CONFIG_CONEXIO_DEVICE_ID
#define AWS_BROKER_HOST  CONFIG_CONEXIO_AWS_BROKER_HOSTNAME
#define AWS_BROKER_PORT  8883
#define SDK_VERSION      "3.2.1"       /* reported as _sdk_version metric    */
#define SLOW_METRIC_EVERY 4            /* at 30-min interval → refresh every 2h */

/* ── MQTT topics ─────────────────────────────────────────────────────────── */
#define TELEMETRY_TOPIC  "devices/" DEVICE_ID "/telemetry"
#define COMMAND_TOPIC    "devices/" DEVICE_ID "/commands"
#define CMD_ACK_TOPIC    "devices/" DEVICE_ID "/commands/ack"
#define CFG_ACK_TOPIC    "devices/" DEVICE_ID "/config/ack"

/* ── MQTT buffers ─────────────────────────────────────────────────────────── */
static uint8_t mqtt_rx_buf[1024];
static uint8_t mqtt_tx_buf[1024];
static uint8_t mqtt_payload_buf[512];

/* ── MQTT client instance ─────────────────────────────────────────────────── */
static struct mqtt_client client;
static struct sockaddr_storage broker_addr;
static bool mqtt_connected = false;

/* ── Runtime config ──────────────────────────────────────────────────────── */
static int telemetry_interval_sec = CONFIG_CONEXIO_TELEMETRY_INTERVAL_SEC;

/* ─────────────────────────────────────────────────────────────────────────── */
/* Reboot reason — read once at boot from hwinfo, sent in first payload       */
/* ─────────────────────────────────────────────────────────────────────────── */
static char g_reboot_reason[16] = "unknown";

static void reboot_reason_init(void)
{
    uint32_t cause = 0;
    if (hwinfo_get_reset_cause(&cause) != 0 || cause == 0) {
        return; /* leave as "unknown" */
    }
    hwinfo_clear_reset_cause(); /* clear so next boot gets a fresh value */

    if      (cause & RESET_WATCHDOG)       strncpy(g_reboot_reason, "watchdog",  sizeof(g_reboot_reason) - 1);
    else if (cause & RESET_CPU_LOCKUP)     strncpy(g_reboot_reason, "lockup",    sizeof(g_reboot_reason) - 1);
    else if (cause & RESET_BROWNOUT)       strncpy(g_reboot_reason, "brownout",  sizeof(g_reboot_reason) - 1);
    else if (cause & RESET_SOFTWARE)       strncpy(g_reboot_reason, "software",  sizeof(g_reboot_reason) - 1);
    else if (cause & RESET_PIN)            strncpy(g_reboot_reason, "pin",       sizeof(g_reboot_reason) - 1);
    else if (cause & RESET_POR)            strncpy(g_reboot_reason, "por",       sizeof(g_reboot_reason) - 1);
    else if (cause & RESET_LOW_POWER_WAKE) strncpy(g_reboot_reason, "wake",      sizeof(g_reboot_reason) - 1);
    g_reboot_reason[sizeof(g_reboot_reason) - 1] = '\0';

    LOG_INF("Reboot reason: %s (0x%08x)", g_reboot_reason, cause);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* LTE session metrics — populated by lte_event_handler                       */
/* ─────────────────────────────────────────────────────────────────────────── */
static struct {
    int64_t  lte_start_ms;      /* k_uptime_get() when LTE connect started   */
    int32_t  connect_time_ms;   /* ms from boot to first LTE registration     */
    int32_t  lte_mode;          /* 7=LTE-M, 9=NB-IoT, 0=unknown              */
    uint32_t cell_id;           /* E-UTRAN cell ID                            */
    uint32_t tac;               /* Tracking Area Code                         */
    int32_t  psm_tau_sec;       /* PSM TAU granted by network (-1 if none)    */
    int32_t  psm_active_sec;    /* PSM active window granted (-1 if none)     */
    float    edrx_interval_ms;  /* eDRX interval (0 if not granted)           */
    uint32_t conn_loss;         /* LTE drop + re-register count since boot    */
    bool     registered;        /* true once first registration happens       */
} g_lte;

/* ─────────────────────────────────────────────────────────────────────────── */
/* MQTT ACK helpers                                                            */
/* ─────────────────────────────────────────────────────────────────────────── */

/**
 * Publish a command ACK to devices/<id>/commands/ack.
 * Updates command status to 'acknowledged' in the dashboard.
 * Payload: { "commandId": "...", "sk": "...", "result": "..." }
 */
static void publish_command_ack(const char *command_id, const char *sk,
                                 const char *result)
{
    if (!mqtt_connected || !command_id) return;

    cJSON *ack = cJSON_CreateObject();
    cJSON_AddStringToObject(ack, "commandId", command_id);
    if (sk)     cJSON_AddStringToObject(ack, "sk",     sk);
    if (result) cJSON_AddStringToObject(ack, "result", result);

    char *json = cJSON_PrintUnformatted(ack);
    cJSON_Delete(ack);
    if (!json) return;

    struct mqtt_publish_param msg = {
        .message.topic.qos        = MQTT_QOS_1_AT_LEAST_ONCE,
        .message.topic.topic.utf8 = (uint8_t *)CMD_ACK_TOPIC,
        .message.topic.topic.size = strlen(CMD_ACK_TOPIC),
        .message.payload.data     = (uint8_t *)json,
        .message.payload.len      = strlen(json),
        .message_id               = (uint16_t)(k_uptime_get_32() & 0xFFFF),
    };
    mqtt_publish(&client, &msg);
    cJSON_free(json);
}

/**
 * Publish a config ACK to devices/<id>/config/ack.
 * Updates config version status to 'applied' in the OTA Config page.
 * Payload: { "configId": "...", "success": true }
 */
static void publish_config_ack(const char *config_id, bool success)
{
    if (!mqtt_connected) return;

    cJSON *ack = cJSON_CreateObject();
    if (config_id) cJSON_AddStringToObject(ack, "configId", config_id);
    cJSON_AddBoolToObject(ack, "success", success);

    char *json = cJSON_PrintUnformatted(ack);
    cJSON_Delete(ack);
    if (!json) return;

    struct mqtt_publish_param msg = {
        .message.topic.qos        = MQTT_QOS_1_AT_LEAST_ONCE,
        .message.topic.topic.utf8 = (uint8_t *)CFG_ACK_TOPIC,
        .message.topic.topic.size = strlen(CFG_ACK_TOPIC),
        .message.payload.data     = (uint8_t *)json,
        .message.payload.len      = strlen(json),
        .message_id               = (uint16_t)(k_uptime_get_32() & 0xFFFF),
    };
    mqtt_publish(&client, &msg);
    cJSON_free(json);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Reboot counter — NVS-persisted, incremented on every boot                  */
/*                                                                             */
/* This is the same counter the SDK auto-adds. In standalone samples it must  */
/* be managed manually and included in the telemetry payload.                 */
/*                                                                             */
/* The cloud tracker.ts detects every increment and records a reboot event    */
/* with a timestamp — visible on Fleet Health → Reboot Tracking tab.         */
/* Works for: power cycles, watchdog resets, firmware crashes, manual REBOOT  */
/* commands, and OTA reboots.                                                  */
/* ─────────────────────────────────────────────────────────────────────────── */

#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>

#define REBOOT_NVS_PARTITION        storage_partition
#define REBOOT_NVS_PARTITION_DEVICE FIXED_PARTITION_DEVICE(REBOOT_NVS_PARTITION)
#define REBOOT_NVS_PARTITION_OFFSET FIXED_PARTITION_OFFSET(REBOOT_NVS_PARTITION)
#define REBOOT_CNT_NVS_ID           1U

static struct nvs_fs   reboot_nvs;
static bool            reboot_nvs_ready = false;
static uint32_t        g_reboot_cnt     = 0;

static void reboot_counter_init(void)
{
    struct flash_pages_info info;
    reboot_nvs.flash_device = REBOOT_NVS_PARTITION_DEVICE;
    reboot_nvs.offset       = REBOOT_NVS_PARTITION_OFFSET;

    if (!device_is_ready(reboot_nvs.flash_device)) {
        LOG_WRN("Reboot counter: flash not ready — counter will reset each boot");
        return;
    }
    flash_get_page_info_by_offs(reboot_nvs.flash_device, reboot_nvs.offset, &info);
    reboot_nvs.sector_size  = info.size;
    reboot_nvs.sector_count = 2U;

    if (nvs_mount(&reboot_nvs) != 0) {
        LOG_WRN("Reboot counter: NVS mount failed — counter will reset each boot");
        return;
    }
    reboot_nvs_ready = true;

    /* Read the previous value (returns -ENOENT on very first boot → stays 0) */
    nvs_read(&reboot_nvs, REBOOT_CNT_NVS_ID, &g_reboot_cnt, sizeof(g_reboot_cnt));
    g_reboot_cnt++;
    nvs_write(&reboot_nvs, REBOOT_CNT_NVS_ID, &g_reboot_cnt, sizeof(g_reboot_cnt));

    LOG_INF("Reboot counter: %u", g_reboot_cnt);
}

static float read_temperature(void)
{
    /* Simulated: random value in [22.0, 30.0] °C with 0.1 resolution */
    uint32_t r = sys_rand32_get() % 81; /* 0-80 → 0.0-8.0 */
    return 22.0f + (float)r * 0.1f;
}

static float read_humidity(void)
{
    /* Simulated: random value in [50.0, 70.0] % with 0.1 resolution */
    uint32_t r = sys_rand32_get() % 201; /* 0-200 → 0.0-20.0 */
    return 50.0f + (float)r * 0.1f;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Timestamp helper — real UTC time via date_time library                      */
/* ─────────────────────────────────────────────────────────────────────────── */

static void get_iso8601(char *buf, size_t buf_len)
{
    int64_t unix_ms = 0;
    int ret = date_time_now(&unix_ms);

    if (ret || unix_ms <= 0) {
        /* NTP not synced yet — fall back to uptime-based placeholder */
        uint32_t s  = (uint32_t)(k_uptime_get() / 1000U);
        uint32_t hh = (s / 3600U) % 100U;
        uint32_t mm = (s % 3600U) / 60U;
        uint32_t ss =  s % 60U;
        snprintf(buf, buf_len,
                 "1970-01-01T%02u:%02u:%02u.000Z", hh, mm, ss);
        return;
    }

    time_t unix_sec = (time_t)(unix_ms / 1000);
    struct tm tm_buf;
    struct tm *t = gmtime_r(&unix_sec, &tm_buf);
    int year = t->tm_year + 1900;
    int ms   = (int)(unix_ms % 1000);

    /* Clamp all fields to their valid ranges so the compiler can prove the
     * snprintf output fits in the 32-byte buffer and suppress -Wformat-truncation. */
    year         = (year < 1970) ? 1970 : ((year > 9999) ? 9999 : year);
    int mon      = (t->tm_mon  + 1);  /* 1-12 */
    int mday     = t->tm_mday;        /* 1-31 */
    int hour     = t->tm_hour;        /* 0-23 */
    int min      = t->tm_min;         /* 0-59 */
    int sec      = t->tm_sec;         /* 0-60 */
    ms           = (ms < 0) ? 0 : ((ms > 999) ? 999 : ms);

    snprintf(buf, buf_len,
             "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             year, mon, mday, hour, min, sec, ms);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Telemetry publish                                                           */
/* ─────────────────────────────────────────────────────────────────────────── */

static int publish_telemetry(void)
{
    char timestamp[32];
    get_iso8601(timestamp, sizeof(timestamp));

    float   temperature = read_temperature();
    float   humidity    = read_humidity();

    /* ── Publish-tier counters ─────────────────────────────────────────── */
    static bool     s_boot_sent  = false;  /* flips true after first publish  */
    static uint16_t s_slow_tick  = 0;      /* wraps at SLOW_METRIC_EVERY      */
    bool emit_boot = !s_boot_sent;
    bool emit_slow = (s_slow_tick == 0);

    /* ── Build JSON payload ───────────────────────────────────────────── */
    cJSON *root    = cJSON_CreateObject();
    cJSON *metrics = cJSON_CreateObject();
    if (!root || !metrics) {
        cJSON_Delete(root);
        LOG_ERR("cJSON_CreateObject returned NULL");
        return -ENOMEM;
    }
    cJSON_AddStringToObject(root, "deviceId",  DEVICE_ID);
    cJSON_AddStringToObject(root, "timestamp", timestamp);
    cJSON_AddItemToObject(root, "metrics", metrics);

    /* ── Application sensors ─────────────────────────────────────────── */
    cJSON_AddNumberToObject(metrics, "temperature", (double)temperature);
    cJSON_AddNumberToObject(metrics, "humidity",    (double)humidity);

    /* ── EVERY-publish modem metrics ─────────────────────────────────── */

    /* _rssi  — RSRP from modem (cached; refreshed every publish) */
    {
        struct modem_param_info mp;
        if (modem_info_params_init(&mp) == 0 &&
            modem_info_params_get(&mp)  == 0) {
            cJSON_AddNumberToObject(metrics, "_rssi",
                                    (double)mp.network.rsrp.value);
        }
    }

    /* _snr — Signal-to-Noise Ratio index (SNR_IDX_TO_DB = x-24 dB) */
    {
        int snr = 0;
        if (modem_info_get_snr(&snr) == 0 && snr != SNR_UNAVAILABLE) {
            cJSON_AddNumberToObject(metrics, "_snr", (double)snr);
        }
    }

    /* _modem_temp — modem die temperature in °C */
    {
        int temp = 0;
        if (modem_info_get_temperature(&temp) == 0) {
            cJSON_AddNumberToObject(metrics, "_modem_temp", (double)temp);
        }
    }

    /* _battery_mv — battery voltage in millivolts */
    {
        int batt_mv = 0;
        if (modem_info_get_batt_voltage(&batt_mv) == 0 && batt_mv > 0) {
            cJSON_AddNumberToObject(metrics, "_battery_mv", (double)batt_mv);
        }
    }

    /* _tx_kb / _rx_kb — data usage since boot */
    {
        int tx_kb = 0, rx_kb = 0;
        if (modem_info_get_connectivity_stats(&tx_kb, &rx_kb) == 0) {
            cJSON_AddNumberToObject(metrics, "_tx_kb", (double)tx_kb);
            cJSON_AddNumberToObject(metrics, "_rx_kb", (double)rx_kb);
        }
    }

    /* _conn_loss — LTE drop + re-register count; always latest value */
    cJSON_AddNumberToObject(metrics, "_conn_loss", (double)g_lte.conn_loss);

    /* _reboot_cnt — always include so cloud can detect every reboot */
    cJSON_AddNumberToObject(metrics, "_reboot_cnt", (double)g_reboot_cnt);

    /* ── BOOT-ONCE metrics ───────────────────────────────────────────── */
    if (emit_boot) {
        cJSON_AddStringToObject(metrics, "_reboot_reason", g_reboot_reason);
        cJSON_AddStringToObject(metrics, "_sdk_version",   SDK_VERSION);

        /* _modem_fw — firmware version string */
        {
            static char fw_buf[MODEM_INFO_FWVER_SIZE] = {0};
            if (fw_buf[0] == '\0') {
                modem_info_get_fw_version(fw_buf, sizeof(fw_buf));
            }
            if (fw_buf[0] != '\0') {
                cJSON_AddStringToObject(metrics, "_modem_fw", fw_buf);
            }
        }

        /* _operator — carrier name (retried until modem attaches) */
        {
            static bool op_sent = false;
            if (!op_sent) {
                char op_buf[MODEM_INFO_SHORT_OP_NAME_SIZE] = {0};
                if (modem_info_get_operator(op_buf, sizeof(op_buf)) == 0
                    && op_buf[0] != '\0') {
                    cJSON_AddStringToObject(metrics, "_operator", op_buf);
                    op_sent = true;
                }
            }
        }

        if (g_lte.lte_mode != 0) {
            cJSON_AddNumberToObject(metrics, "_lte_mode",
                                    (double)g_lte.lte_mode);
        }
        if (g_lte.connect_time_ms > 0) {
            cJSON_AddNumberToObject(metrics, "_lte_connect_ms",
                                    (double)g_lte.connect_time_ms);
        }
        if (g_lte.psm_tau_sec > 0) {
            cJSON_AddNumberToObject(metrics, "_psm_tau_sec",
                                    (double)g_lte.psm_tau_sec);
        }
        if (g_lte.psm_active_sec >= 0) {
            cJSON_AddNumberToObject(metrics, "_psm_active_sec",
                                    (double)g_lte.psm_active_sec);
        }
        if (g_lte.edrx_interval_ms > 0.0f) {
            cJSON_AddNumberToObject(metrics, "_edrx_ms",
                                    (double)g_lte.edrx_interval_ms);
        }

        s_boot_sent = true;
    }

    /* ── SLOW metrics (every SLOW_METRIC_EVERY publishes) ──────────────── */
    if (emit_slow) {
        /* _lte_band — changes only on cell handover */
        {
            uint8_t band = 0;
            if (modem_info_get_current_band(&band) == 0
                && band != BAND_UNAVAILABLE) {
                cJSON_AddNumberToObject(metrics, "_lte_band", (double)band);
            }
        }

        /* _cell_id / _tac — change when device moves between cells */
        if (g_lte.cell_id != 0) {
            cJSON_AddNumberToObject(metrics, "_cell_id",
                                    (double)g_lte.cell_id);
        }
        if (g_lte.tac != 0) {
            cJSON_AddNumberToObject(metrics, "_tac", (double)g_lte.tac);
        }
    }

    /* Advance slow tick */
    s_slow_tick = (s_slow_tick + 1) % SLOW_METRIC_EVERY;

    /* ── Serialise and publish ──────────────────────────────────────────── */
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        LOG_ERR("cJSON_PrintUnformatted returned NULL");
        return -ENOMEM;
    }

    struct mqtt_publish_param msg = {
        .message.topic.qos        = MQTT_QOS_1_AT_LEAST_ONCE,
        .message.topic.topic.utf8 = (uint8_t *)TELEMETRY_TOPIC,
        .message.topic.topic.size = strlen(TELEMETRY_TOPIC),
        .message.payload.data     = (uint8_t *)payload,
        .message.payload.len      = (uint32_t)strlen(payload),
        .message_id               = (uint16_t)(k_uptime_get_32() & 0xFFFF),
        .dup_flag                 = 0,
        .retain_flag              = 0,
    };

    int ret = mqtt_publish(&client, &msg);
    cJSON_free(payload);

    if (ret) {
        LOG_ERR("mqtt_publish failed (err %d)", ret);
    } else {
        LOG_INF("Telemetry published: temp=%d.%d hum=%d.%d rssi=%s",
                (int)temperature,
                (int)((temperature - (int)temperature) * 10),
                (int)humidity,
                (int)((humidity    - (int)humidity)    * 10),
                emit_boot ? "(boot metrics included)" : "");
    }
    return ret;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Incoming command handler                                                    */
/* ─────────────────────────────────────────────────────────────────────────── */

static void handle_command(const char *json_str, size_t len)
{
    char buf[512];
    size_t copy_len = MIN(len, sizeof(buf) - 1);
    memcpy(buf, json_str, copy_len);
    buf[copy_len] = '\0';

    LOG_INF("Incoming message: %s", buf);

    cJSON *msg = cJSON_Parse(buf);
    if (!msg) {
        LOG_WRN("Failed to parse incoming JSON");
        return;
    }

    const cJSON *type_item = cJSON_GetObjectItem(msg, "type");
    const char  *type      = cJSON_GetStringValue(type_item);

    if (!type) {
        cJSON_Delete(msg);
        return;
    }

    /* ── OTA config push from the dashboard ──────────────────────────────── */
    if (strcmp(type, "config") == 0) {
        const cJSON *config    = cJSON_GetObjectItem(msg, "config");
        const char  *config_id = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "configId"));

        if (config) {
            LOG_INF("Applying OTA config (version %d)",
                    (int)cJSON_GetNumberValue(cJSON_GetObjectItem(msg, "version")));

            const cJSON *interval_item =
                cJSON_GetObjectItem(config, "telemetryIntervalSec");
            if (cJSON_IsNumber(interval_item)) {
                int new_interval = (int)interval_item->valuedouble;
                if (new_interval >= 10 && new_interval <= 3600) {
                    telemetry_interval_sec = new_interval;
                    LOG_INF("Telemetry interval updated to %ds",
                            telemetry_interval_sec);
                }
            }
        }

        publish_config_ack(config_id, true);
        cJSON_Delete(msg);
        return;
    }

    /* ── Device command ──────────────────────────────────────────────────── */
    if (strcmp(type, "command") == 0) {
        const cJSON *cmd_item   = cJSON_GetObjectItem(msg, "command");
        const char  *cmd        = cJSON_GetStringValue(cmd_item);
        const char  *command_id = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "commandId"));
        const char  *sk         = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "sk"));
        const cJSON *payload    = cJSON_GetObjectItem(msg, "payload");

        if (!cmd) {
            cJSON_Delete(msg);
            return;
        }

        LOG_INF("Command received: %s (id: %s)", cmd, command_id ? command_id : "?");

        if (strcmp(cmd, "REBOOT") == 0) {
            publish_command_ack(command_id, sk, "rebooting");
            cJSON_Delete(msg);
            k_sleep(K_MSEC(500));
            sys_reboot(SYS_REBOOT_COLD);
            return;

        } else if (strcmp(cmd, "FAN_ON") == 0) {
            const cJSON *speed_item = payload
                ? cJSON_GetObjectItem(payload, "speed") : NULL;
            int speed = cJSON_IsNumber(speed_item)
                ? (int)speed_item->valuedouble : 100;
            LOG_INF("FAN_ON at speed %d%%", speed);
            publish_command_ack(command_id, sk, "fan_on");

        } else if (strcmp(cmd, "FAN_OFF") == 0) {
            LOG_INF("FAN_OFF");
            publish_command_ack(command_id, sk, "fan_off");

        } else if (strcmp(cmd, "SET_INTERVAL") == 0) {
            const cJSON *interval_item = payload
                ? cJSON_GetObjectItem(payload, "interval") : NULL;
            if (cJSON_IsNumber(interval_item)) {
                int new_sec = (int)interval_item->valuedouble;
                if (new_sec >= 10 && new_sec <= 3600) {
                    telemetry_interval_sec = new_sec;
                    LOG_INF("Telemetry interval set to %ds", telemetry_interval_sec);
                    publish_command_ack(command_id, sk, "interval_set");
                } else {
                    publish_command_ack(command_id, sk, NULL);
                }
            } else {
                publish_command_ack(command_id, sk, NULL);
            }

        } else if (strcmp(cmd, "CALIBRATE") == 0) {
            LOG_INF("Calibrating sensors...");
            publish_command_ack(command_id, sk, "calibrated");

        } else {
            LOG_WRN("Unknown command: %s", cmd);
            publish_command_ack(command_id, sk, "unknown_command");
        }
    }

    cJSON_Delete(msg);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* MQTT event handler                                                          */
/* ─────────────────────────────────────────────────────────────────────────── */

static void mqtt_event_handler(struct mqtt_client *c,
                               const struct mqtt_evt *evt)
{
    int ret;

    switch (evt->type) {

    case MQTT_EVT_CONNACK:
        if (evt->result == 0) {
            LOG_INF("MQTT connected to %s", AWS_BROKER_HOST);
            mqtt_connected = true;

            /*
             * Subscribe to both topics the cloud publishes to this device:
             *   commands — commands from Commands page and Schedules page
             *   config   — OTA Config pushes from the OTA Config page
             *
             * Both arrive as JSON with a "type" field ("command" or "config").
             * handle_command() dispatches to the right handler based on type.
             */
            struct mqtt_topic subs_topics[2] = {
                {
                    .topic = {
                        .utf8 = (uint8_t *)COMMAND_TOPIC,
                        .size = strlen(COMMAND_TOPIC),
                    },
                    .qos = MQTT_QOS_1_AT_LEAST_ONCE,
                },
                {
                    .topic = {
                        .utf8 = (uint8_t *)"devices/" DEVICE_ID "/config",
                        .size = strlen("devices/" DEVICE_ID "/config"),
                    },
                    .qos = MQTT_QOS_1_AT_LEAST_ONCE,
                },
            };
            const struct mqtt_subscription_list sub_list = {
                .list       = subs_topics,
                .list_count = ARRAY_SIZE(subs_topics),
                .message_id = 1,
            };
            ret = mqtt_subscribe(c, &sub_list);
            if (ret) {
                LOG_ERR("mqtt_subscribe failed (err %d)", ret);
            } else {
                LOG_INF("Subscribed to: %s and devices/" DEVICE_ID "/config",
                        COMMAND_TOPIC);
            }
        } else {
            LOG_ERR("MQTT CONNACK error %d", evt->result);
        }
        break;

    case MQTT_EVT_DISCONNECT:
        LOG_WRN("MQTT disconnected (result %d)", evt->result);
        mqtt_connected = false;
        break;

    case MQTT_EVT_PUBLISH: {
        /* Incoming message from the dashboard */
        const struct mqtt_publish_param *p = &evt->param.publish;
        size_t payload_len = p->message.payload.len;

        if (payload_len >= sizeof(mqtt_payload_buf)) {
            LOG_WRN("Incoming payload too large (%zu bytes), truncating",
                    payload_len);
            payload_len = sizeof(mqtt_payload_buf) - 1;
        }

        ret = mqtt_read_publish_payload_blocking(c, mqtt_payload_buf,
                                                 payload_len);
        if (ret < 0) {
            LOG_ERR("mqtt_read_publish_payload failed (err %d)", ret);
            break;
        }
        mqtt_payload_buf[ret] = '\0';

        handle_command((char *)mqtt_payload_buf, ret);

        /* Send PUBACK for QoS 1 */
        if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
            struct mqtt_puback_param puback = {
                .message_id = p->message_id,
            };
            mqtt_publish_qos1_ack(c, &puback);
        }
        break;
    }

    case MQTT_EVT_PUBACK:
        LOG_DBG("PUBACK received (id %d)", evt->param.puback.message_id);
        break;

    case MQTT_EVT_SUBACK:
        LOG_INF("SUBACK received — subscribed to command topic");
        break;

    case MQTT_EVT_PINGRESP:
        LOG_DBG("PINGRESP received");
        break;

    default:
        LOG_DBG("Unhandled MQTT event: %d", evt->type);
        break;
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Broker address resolution                                                   */
/* ─────────────────────────────────────────────────────────────────────────── */

static int resolve_broker_addr(void)
{
    struct zsock_addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct zsock_addrinfo *result;
    char port_str[6];

    snprintf(port_str, sizeof(port_str), "%d", AWS_BROKER_PORT);

    int ret = zsock_getaddrinfo(AWS_BROKER_HOST, port_str, &hints, &result);
    if (ret) {
        LOG_ERR("zsock_getaddrinfo(%s) failed (err %d)", AWS_BROKER_HOST, ret);
        return -ENOENT;
    }

    memcpy(&broker_addr, result->ai_addr, result->ai_addrlen);
    zsock_freeaddrinfo(result);

    LOG_INF("Broker %s resolved", AWS_BROKER_HOST);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* MQTT client setup                                                           */
/* ─────────────────────────────────────────────────────────────────────────── */

static sec_tag_t sec_tags[] = { AWS_CA_TAG, AWS_CERT_TAG, AWS_KEY_TAG };

static void mqtt_client_setup(void)
{
    mqtt_client_init(&client);

    client.broker        = &broker_addr;
    client.evt_cb        = mqtt_event_handler;

    client.client_id.utf8 = (uint8_t *)DEVICE_ID;
    client.client_id.size = strlen(DEVICE_ID);

    client.password = NULL;
    client.user_name = NULL;

    client.protocol_version = MQTT_VERSION_3_1_1;
    client.keepalive        = CONFIG_CONEXIO_MQTT_KEEPALIVE_SEC;
    client.clean_session    = 1;

    client.rx_buf      = mqtt_rx_buf;
    client.rx_buf_size = sizeof(mqtt_rx_buf);
    client.tx_buf      = mqtt_tx_buf;
    client.tx_buf_size = sizeof(mqtt_tx_buf);

    /* TLS transport */
    client.transport.type = MQTT_TRANSPORT_SECURE;
    struct mqtt_sec_config *tls = &client.transport.tls.config;
    tls->peer_verify   = TLS_PEER_VERIFY_REQUIRED;
    tls->cipher_count  = 0; /* use modem defaults */
    tls->cipher_list   = NULL;
    tls->sec_tag_list  = sec_tags;
    tls->sec_tag_count = ARRAY_SIZE(sec_tags);
    tls->hostname      = AWS_BROKER_HOST;
    tls->session_cache = TLS_SESSION_CACHE_DISABLED;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* LTE event handler                                                           */
/* ─────────────────────────────────────────────────────────────────────────── */

static K_SEM_DEFINE(lte_ready, 0, 1);

static void lte_event_handler(const struct lte_lc_evt *const evt)
{
    switch (evt->type) {
    case LTE_LC_EVT_NW_REG_STATUS:
        if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
            evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
            LOG_INF("LTE registered (%s)",
                    evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME
                    ? "home" : "roaming");
            if (!g_lte.registered) {
                /* First registration — record connect time */
                g_lte.connect_time_ms = (int32_t)(k_uptime_get() - g_lte.lte_start_ms);
                g_lte.registered      = true;
                LOG_INF("LTE connect time: %d ms", g_lte.connect_time_ms);
            } else {
                /* Re-registration after a drop */
                g_lte.conn_loss++;
                LOG_INF("LTE re-registered (conn_loss=%u)", g_lte.conn_loss);
            }
            k_sem_give(&lte_ready);
        } else if (evt->nw_reg_status == LTE_LC_NW_REG_NOT_REGISTERED) {
            if (g_lte.registered) {
                LOG_WRN("LTE dropped");
            }
        }
        break;

    case LTE_LC_EVT_LTE_MODE_UPDATE:
        g_lte.lte_mode = (int32_t)evt->lte_mode; /* 7=LTE-M, 9=NB-IoT */
        LOG_DBG("LTE mode: %d", g_lte.lte_mode);
        break;

    case LTE_LC_EVT_CELL_UPDATE:
        g_lte.cell_id = evt->cell.id;
        g_lte.tac     = evt->cell.tac;
        LOG_DBG("Cell update: id=0x%08x tac=0x%04x", g_lte.cell_id, g_lte.tac);
        break;

    case LTE_LC_EVT_PSM_UPDATE:
        g_lte.psm_tau_sec    = evt->psm_cfg.tau;
        g_lte.psm_active_sec = evt->psm_cfg.active_time;
        LOG_DBG("PSM: TAU=%d, active=%d", g_lte.psm_tau_sec, g_lte.psm_active_sec);
        break;

    case LTE_LC_EVT_EDRX_UPDATE:
        g_lte.edrx_interval_ms = evt->edrx_cfg.edrx * 1000.0f;
        LOG_DBG("eDRX: %.0f ms", (double)g_lte.edrx_interval_ms);
        break;

    default:
        break;
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Main                                                                        */
/* ─────────────────────────────────────────────────────────────────────────── */

int main(void)
{
    int ret;

    LOG_INF("=== Conexio Console MQTT sample ===");
    LOG_INF("Device ID : %s", DEVICE_ID);
    LOG_INF("Broker    : %s:%d", AWS_BROKER_HOST, AWS_BROKER_PORT);
    LOG_INF("Interval  : %ds", telemetry_interval_sec);

    /* 1. Init modem library */
    ret = nrf_modem_lib_init();
    if (ret) {
        LOG_ERR("nrf_modem_lib_init failed (%d)", ret);
        return -1;
    }

    /* 2. Take modem offline so we can write credentials safely */
    lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE);

    /* 3. Provision TLS certificates into modem (skips if already present) */
    ret = aws_certs_provision();
    if (ret) {
        LOG_ERR("Certificate provisioning failed (%d) — halting", ret);
        return -1;
    }

    /* 4. Initialise reboot counter + reboot reason */
    reboot_counter_init();
    reboot_reason_init();
    LOG_INF("Reboot counter: %u  Reason: %s", g_reboot_cnt, g_reboot_reason);

    /* 5. Initialise modem info + connectivity stats */
    ret = modem_info_init();
    if (ret) {
        LOG_WRN("modem_info_init failed (%d) — some metrics unavailable", ret);
    }
    if (modem_info_connectivity_stats_init() != 0) {
        LOG_WRN("connectivity_stats_init failed — _tx_kb/_rx_kb unavailable");
    }

    /* 6. Connect to LTE — record start time for _lte_connect_ms */
    LOG_INF("Connecting to LTE...");
    g_lte.lte_start_ms = k_uptime_get();
    g_lte.psm_tau_sec  = -1;
    g_lte.psm_active_sec = -1;
    ret = lte_lc_connect_async(lte_event_handler);
    if (ret) {
        LOG_ERR("lte_lc_connect_async failed (%d)", ret);
        return -1;
    }

    /* Wait up to 90 seconds for LTE registration */
    ret = k_sem_take(&lte_ready, K_SECONDS(90));
    if (ret) {
        LOG_ERR("LTE registration timed out");
        return -1;
    }

    /* 4. Sync time via NTP — non-blocking, runs in background.
     * date_time_now() will return the real UTC time once sync completes.
     * First few telemetry packets may use the uptime fallback. */
    LOG_INF("Requesting NTP time sync...");
    date_time_update_async(NULL);
    k_sleep(K_SECONDS(3)); /* give NTP a moment before first publish */

    /* 5. Resolve broker address */
    ret = resolve_broker_addr();
    if (ret) {
        return -1;
    }

    /* 6. Configure MQTT client */
    mqtt_client_setup();

    /* 7. Main loop — connect, publish, handle commands */
    /* Negative initial value forces an immediate publish on the first connect. */
    int64_t last_publish_ms = -(int64_t)telemetry_interval_sec * 1000;

    while (1) {
        if (!mqtt_connected) {
            LOG_INF("Connecting to AWS IoT Core...");
            /* Re-initialise client struct before each connect — clears stale
             * TLS socket state from the previous session. */
            mqtt_client_setup();
            ret = mqtt_connect(&client);
            if (ret) {
                LOG_ERR("mqtt_connect failed (%d) — retrying in 10s", ret);
                k_sleep(K_SECONDS(10));
                continue;
            }
            /* Reset fd after new connection */
        }

        /* Drive MQTT (process incoming + keepalive) — poll with short timeout */
        struct zsock_pollfd fds = {
            .fd     = client.transport.tls.sock,
            .events = ZSOCK_POLLIN,
        };
        ret = zsock_poll(&fds, 1, 1000); /* 1000 ms */
        if (ret > 0 && (fds.revents & ZSOCK_POLLIN)) {
            ret = mqtt_input(&client);
            if (ret) {
                LOG_WRN("mqtt_input error (%d)", ret);
                mqtt_disconnect(&client, NULL);
                mqtt_connected = false;
                continue;
            }
        }

        if (fds.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL)) {
            LOG_WRN("poll error: revents=0x%x", fds.revents);
            mqtt_disconnect(&client, NULL);
            mqtt_connected = false;
            continue;
        }

        ret = mqtt_live(&client);
        if (ret && ret != -EAGAIN) {
            LOG_WRN("mqtt_live error (%d)", ret);
            mqtt_disconnect(&client, NULL);
            mqtt_connected = false;
            continue;
        }

        /* Publish telemetry on schedule */
        int64_t now_ms = k_uptime_get();
        if (mqtt_connected &&
            (now_ms - last_publish_ms >= (int64_t)telemetry_interval_sec * 1000)) {
            publish_telemetry();
            last_publish_ms = now_ms;

            /* ── Disconnect after publish to save battery ──────────────────
             * The LTE radio is the dominant power consumer.  Staying connected
             * for 30 minutes just to send one packet wastes battery.
             * Pattern: connect → subscribe → publish → disconnect → sleep.
             * The next loop iteration reconnects automatically when the next
             * publish is due.  Each reconnect costs ~5–10 s of LTE activity
             * which is far less than 30 min of idle radio draw. */
            LOG_INF("Disconnecting to save battery — next publish in %ds",
                    telemetry_interval_sec);
            mqtt_disconnect(&client, NULL);
            mqtt_connected = false;

            /* Sleep until it's nearly time for the next publish.
             * Subtract 15s to allow time for reconnect + TLS handshake.
             * Guard: don't sleep negative if interval is very short (testing). */
            int sleep_sec = telemetry_interval_sec - 15;
            if (sleep_sec > 0) {
                k_sleep(K_SECONDS(sleep_sec));
            }
        }
    }
}
