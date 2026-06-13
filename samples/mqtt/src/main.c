/*
 * main.c — Conexio Console MQTT telemetry sample
 *
 * nRF Connect SDK v3.2.1 / nRF91xx
 *
 * Flow:
 *   1. Provision TLS certificates into modem (once)
 *   2. Connect to LTE
 *   3. Sync time via NTP
 *   4. Connect to AWS IoT Core over MQTT/TLS (port 8883)
 *   5. Subscribe to commands topic
 *   6. Publish telemetry every CONFIG_CONEXIO_TELEMETRY_INTERVAL_SEC seconds
 *   7. Handle incoming commands / OTA config pushes from the dashboard
 */

#include <zephyr/kernel.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <modem/lte_lc.h>
#include <modem/modem_info.h>
#include <date_time.h>
#include <cJSON.h>
#include <cJSON_os.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <stdio.h>
#include <string.h>

#include "aws_certs.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

/* ── Configuration ───────────────────────────────────────────────────────── */
#define DEVICE_ID       CONFIG_CONEXIO_DEVICE_ID
#define AWS_BROKER_HOST CONFIG_CONEXIO_AWS_BROKER_HOSTNAME
#define AWS_BROKER_PORT 8883

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

/* ── Runtime config (can be updated via OTA config push) ─────────────────── */
static int telemetry_interval_sec = CONFIG_CONEXIO_TELEMETRY_INTERVAL_SEC;

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
    cJSON_FreeString(json);
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
    cJSON_FreeString(json);
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
        return -1;
    }
    flash_get_page_info_by_offs(reboot_nvs.flash_device, reboot_nvs.offset, &info);
    reboot_nvs.sector_size  = info.size;
    reboot_nvs.sector_count = 2U;

    if (nvs_mount(&reboot_nvs) != 0) {
        LOG_WRN("Reboot counter: NVS mount failed — counter will reset each boot");
        return -1;
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
    /* TODO: replace with your sensor driver call, e.g.:
     *   struct sensor_value val;
     *   sensor_sample_fetch(dev);
     *   sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &val);
     *   return sensor_value_to_double(&val);
     */
    return 22.5f; /* placeholder */
}

static float read_humidity(void)
{
    /* TODO: replace with your sensor driver call */
    return 61.0f; /* placeholder */
}

static int16_t read_rssi(void)
{
    /*
     * Read RSSI from the modem using modem_info.
     * Returns dBm value (e.g. -68).
     * This populates the Signal Quality page in the dashboard.
     */
    struct modem_param_info modem_param;
    int ret = modem_info_params_get(&modem_param);
    if (ret) {
        return INT16_MIN; /* unavailable */
    }
    return (int16_t)modem_param.network.rsrp.value;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* ISO-8601 timestamp helper                                                   */
/* ─────────────────────────────────────────────────────────────────────────── */

static void get_iso8601(char *buf, size_t buf_len)
{
    int64_t unix_ms;
    int ret = date_time_now(&unix_ms);
    if (ret) {
        /* Fallback: use uptime */
        snprintf(buf, buf_len, "1970-01-01T00:00:%02lld.000Z",
                 (long long)(k_uptime_get() / 1000));
        return -1;
    }

    time_t unix_sec = (time_t)(unix_ms / 1000);
    struct tm *t = gmtime(&unix_sec);
    snprintf(buf, buf_len,
             "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec,
             (int)(unix_ms % 1000));
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Telemetry publish                                                           */
/* ─────────────────────────────────────────────────────────────────────────── */

static int publish_telemetry(void)
{
    char timestamp[32];
    get_iso8601(timestamp, sizeof(timestamp));

    float temperature = read_temperature();
    float humidity    = read_humidity();
    int16_t rssi      = read_rssi();

    /* Build JSON payload matching the TelemetryPayload type:
     * {
     *   "deviceId": "my-nrf-device",
     *   "timestamp": "2026-06-03T10:30:00.000Z",
     *   "metrics": {
     *     "temperature": 22.5,
     *     "humidity": 61.0,
     *     "_rssi": -68        ← enables Signal Quality page in dashboard
     *   }
     * }
     */
    cJSON *root    = cJSON_CreateObject();
    cJSON *metrics = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "deviceId",  DEVICE_ID);
    cJSON_AddStringToObject(root, "timestamp", timestamp);
    cJSON_AddItemToObject(root, "metrics", metrics);
    cJSON_AddNumberToObject(metrics, "temperature", (double)temperature);
    cJSON_AddNumberToObject(metrics, "humidity",    (double)humidity);
    if (rssi != INT16_MIN) {
        cJSON_AddNumberToObject(metrics, "_rssi", (double)rssi);
    }
    /* _reboot_cnt: monotonically-increasing counter — cloud detects every
     * increment as a reboot event (Fleet Health → Reboot Tracking tab) */
    cJSON_AddNumberToObject(metrics, "_reboot_cnt", (double)g_reboot_cnt);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!payload) {
        LOG_ERR("Failed to serialise telemetry JSON");
        return -ENOMEM;
    }

    struct mqtt_publish_param msg = {
        .message.topic.qos        = MQTT_QOS_1_AT_LEAST_ONCE,
        .message.topic.topic.utf8 = (uint8_t *)TELEMETRY_TOPIC,
        .message.topic.topic.size = strlen(TELEMETRY_TOPIC),
        .message.payload.data     = (uint8_t *)payload,
        .message.payload.len      = strlen(payload),
        .message_id               = (uint16_t)(k_uptime_get_32() & 0xFFFF),
        .dup_flag                 = 0,
        .retain_flag              = 0,
    };

    int ret = mqtt_publish(&client, &msg);
    if (ret) {
        LOG_ERR("mqtt_publish failed (err %d)", ret);
    } else {
        LOG_INF("Telemetry published: temp=%.1f hum=%.1f rssi=%d",
                (double)temperature, (double)humidity, (int)rssi);
    }

    cJSON_FreeString(payload);
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
        return -1;
    }

    const cJSON *type_item = cJSON_GetObjectItem(msg, "type");
    const char  *type      = cJSON_GetStringValue(type_item);

    if (!type) {
        cJSON_Delete(msg);
        return -1;
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
            /* Add handling for other config keys here */
        }

        /* ACK the config push — updates status to 'applied' in dashboard */
        publish_config_ack(config_id, true);

        cJSON_Delete(msg);
        return -1;
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
            return -1;
        }

        LOG_INF("Command received: %s (id: %s)", cmd, command_id ? command_id : "?");

        if (strcmp(cmd, "REBOOT") == 0) {
            publish_command_ack(command_id, sk, "rebooting");
            cJSON_Delete(msg);
            k_sleep(K_MSEC(500));
            sys_reboot(SYS_REBOOT_COLD);
            return -1;

        } else if (strcmp(cmd, "FAN_ON") == 0) {
            const cJSON *speed_item = payload
                ? cJSON_GetObjectItem(payload, "speed") : NULL;
            int speed = cJSON_IsNumber(speed_item)
                ? (int)speed_item->valuedouble : 100;
            LOG_INF("FAN_ON at speed %d%%", speed);
            /* TODO: gpio_pin_set(fan_dev, FAN_PIN, 1); */
            publish_command_ack(command_id, sk, "fan_on");

        } else if (strcmp(cmd, "FAN_OFF") == 0) {
            LOG_INF("FAN_OFF");
            /* TODO: gpio_pin_set(fan_dev, FAN_PIN, 0); */
            publish_command_ack(command_id, sk, "fan_off");

        } else if (strcmp(cmd, "SET_INTERVAL") == 0) {
            /*
             * interval payload value is in SECONDS (consistent with
             * the OTA Config page telemetryIntervalSec setting).
             * Example: {"interval": 120} sets a 2-minute interval.
             */
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
            /* TODO: trigger your sensor calibration routine */
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
    struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *result;
    char port_str[6];

    snprintf(port_str, sizeof(port_str), "%d", AWS_BROKER_PORT);

    int ret = getaddrinfo(AWS_BROKER_HOST, port_str, &hints, &result);
    if (ret) {
        LOG_ERR("getaddrinfo(%s) failed (err %d)", AWS_BROKER_HOST, ret);
        return -ENOENT;
    }

    memcpy(&broker_addr, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);

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
            k_sem_give(&lte_ready);
        } else if (evt->nw_reg_status == LTE_LC_NW_REG_NOT_REGISTERED) {
            LOG_WRN("LTE not registered");
        }
        break;
    case LTE_LC_EVT_PSM_UPDATE:
        LOG_DBG("PSM: TAU=%d, active=%d",
                evt->psm_cfg.tau, evt->psm_cfg.active_time);
        break;
    case LTE_LC_EVT_EDRX_UPDATE:
        LOG_DBG("eDRX updated");
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

    /* 1. Provision TLS certificates into modem */
    ret = aws_certs_provision();
    if (ret) {
        LOG_ERR("Certificate provisioning failed (%d) — halting", ret);
        return -1;
    }

    /* 2. Initialise reboot counter (NVS-persisted across power cycles) */
    reboot_counter_init();

    /* 3. Initialise modem info (for RSSI) */
    ret = modem_info_init();
    if (ret) {
        LOG_WRN("modem_info_init failed (%d) — RSSI unavailable", ret);
    }

    /* 3. Connect to LTE
     * NCS v3.2.1: lte_lc_init_and_connect_async() removed — use lte_lc_connect_async() */
    LOG_INF("Connecting to LTE...");
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

    /* 4. Sync time via NTP */
    LOG_INF("Syncing time via NTP...");
    date_time_update_async(NULL);
    k_sleep(K_SECONDS(3)); /* give NTP a moment */

    /* 5. Resolve broker address */
    ret = resolve_broker_addr();
    if (ret) {
        return -1;
    }

    /* 6. Configure MQTT client */
    mqtt_client_setup();

    /* 7. Main loop — connect, publish, handle commands */
    while (1) {
        if (!mqtt_connected) {
            LOG_INF("Connecting to AWS IoT Core...");
            ret = mqtt_connect(&client);
            if (ret) {
                LOG_ERR("mqtt_connect failed (%d) — retrying in 10s", ret);
                k_sleep(K_SECONDS(10));
                continue;
            }
        }

        /* Drive MQTT (process incoming + keepalive) — poll with short timeout */
        struct zsock_pollfd fds = {
            .fd     = client.transport.tls.sock,
            .events = ZSOCK_POLLIN,
        };
        ret = zsock_poll(&fds, 1, K_SECONDS(1).ticks);
        if (ret > 0 && (fds.revents & ZSOCK_POLLIN)) {
            ret = mqtt_input(&client);
            if (ret) {
                LOG_WRN("mqtt_input error (%d)", ret);
                /* NCS v3.2.1: mqtt_disconnect() takes 2 args */
                mqtt_disconnect(&client, NULL);
                continue;
            }
        }

        ret = mqtt_live(&client);
        if (ret && ret != -EAGAIN) {
            LOG_WRN("mqtt_live error (%d)", ret);
            /* NCS v3.2.1: mqtt_disconnect() takes 2 args */
            mqtt_disconnect(&client, NULL);
            continue;
        }

        /* Publish telemetry on schedule */
        static int64_t last_publish_ms = 0;
        int64_t now_ms = k_uptime_get();
        if (now_ms - last_publish_ms >= (int64_t)telemetry_interval_sec * 1000) {
            if (mqtt_connected) {
                publish_telemetry();
                last_publish_ms = now_ms;
            }
        }
    }
}
