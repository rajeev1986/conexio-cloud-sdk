/*
 * main.c — Conexio Console Fleet Provisioning sample
 *
 * nRF Connect SDK v3.2.1 / nRF91xx
 *
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │  ONE BINARY — ships to the entire fleet.                                │
 * │  No per-device certificate, no per-device build.                        │
 * │  The device provisions itself on first boot and then operates normally. │
 * └─────────────────────────────────────────────────────────────────────────┘
 *
 * Boot flow:
 *
 *   First boot (device not yet provisioned):
 *   ─────────────────────────────────────────
 *   1.  Provision claim (bootstrap) certs into modem  — embedded in firmware
 *   2.  Connect to LTE
 *   3.  Read IMEI from modem  →  derive device ID: "nrf-imei-<15 digits>"
 *   4.  MQTT connect using claim creds (tags 10-12)
 *   5.  Call AWS CreateKeysAndCertificate  →  receive unique cert + key
 *   6.  Call AWS RegisterThing            →  Thing created, cert activated
 *   7.  Write unique cert + key into modem (tags 20-22)
 *   8.  Persist "provisioned" flag in flash via Settings
 *   9.  Reboot  →  enters normal operation on next boot
 *
 *   Every subsequent boot (already provisioned):
 *   ────────────────────────────────────────────
 *   1.  Check Settings flag / modem for device cert  →  skip provisioning
 *   2.  Connect to LTE
 *   3.  MQTT connect using device creds (tags 20-22)
 *   4.  Subscribe to commands topic
 *   5.  Publish telemetry every CONFIG_CONEXIO_TELEMETRY_INTERVAL_SEC seconds
 *   6.  Handle incoming commands, OTA config pushes, scheduled actions
 */

#include <zephyr/kernel.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/settings/settings.h>
#include <modem/lte_lc.h>
#include <modem/modem_info.h>
#include <date_time.h>
#include <cJSON.h>
#include <cJSON_os.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <stdio.h>
#include <string.h>

#include "cert_store.h"
#include "provision.h"
#include "uart_commissioning.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

/* ── Device ID ───────────────────────────────────────────────────────────── */
/* Derived from modem IMEI at runtime — no prj.conf edit required.           */
#define DEVICE_ID_MAX_LEN 32
static char g_device_id[DEVICE_ID_MAX_LEN]; /* "nrf-imei-<15 digits>" */

/* ── AWS endpoint ────────────────────────────────────────────────────────── */
#define AWS_BROKER_HOST CONFIG_CONEXIO_AWS_BROKER_HOSTNAME
#define AWS_BROKER_PORT 8883

/* ── MQTT topics (built at runtime using g_device_id) ────────────────────── */
static char telemetry_topic[64];
static char command_topic[64];
static char config_topic[64];   /* devices/<id>/config — OTA Config pushes */

/* ── MQTT buffers ─────────────────────────────────────────────────────────── */
static uint8_t mqtt_rx_buf[1024];
static uint8_t mqtt_tx_buf[1024];
static uint8_t mqtt_payload_buf[512];

/* ── MQTT client ─────────────────────────────────────────────────────────── */
static struct mqtt_client client;
static struct sockaddr_storage broker_addr;
static bool mqtt_connected = false;

/* ── Runtime config (updated via OTA config push) ────────────────────────── */
static int telemetry_interval_sec = CONFIG_CONEXIO_TELEMETRY_INTERVAL_SEC;

/* ── Provisioning state (persisted in flash) ─────────────────────────────── */
#define SETTINGS_KEY "prov/done"
static bool g_provisioning_done = false;

/* ─────────────────────────────────────────────────────────────────────────── */
/* Settings load callback — reads persisted provisioning flag from flash      */
/* ─────────────────────────────────────────────────────────────────────────── */

static int settings_load_cb(const char *key, size_t len,
                             settings_read_cb read_cb, void *cb_arg,
                             void *param)
{
    ARG_UNUSED(param);

    if (strcmp(key, "done") == 0) {
        bool val = false;
        ssize_t ret = read_cb(cb_arg, &val, sizeof(val));
        if (ret > 0) {
            g_provisioning_done = val;
            LOG_DBG("Settings: prov/done = %d", (int)val);
        }
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(prov_handler, "prov", NULL,
                                settings_load_cb, NULL, NULL);

static int persist_provisioning_done(void)
{
    bool val = true;
    int ret = settings_save_one(SETTINGS_KEY, &val, sizeof(val));
    if (ret) {
        LOG_WRN("settings_save_one failed (err %d) — flag not persisted", ret);
    } else {
        LOG_INF("Provisioning flag persisted to flash");
    }
    return ret;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* IMEI → Device ID                                                            */
/* ─────────────────────────────────────────────────────────────────────────── */

static int derive_device_id(void)
{
    char imei[16] = {0}; /* 15 digits + null */
    int ret = modem_info_init();
    if (ret) {
        LOG_WRN("modem_info_init failed (err %d)", ret);
        /* Fallback to a fixed ID — only acceptable for development */
        strncpy(g_device_id, "000000000000000", sizeof(g_device_id) - 1);
        return 0;
    }

    struct modem_param_info modem_param;
    ret = modem_info_params_get(&modem_param);
    if (ret) {
        LOG_WRN("modem_info_params_get failed (err %d) — using fallback ID", ret);
        strncpy(g_device_id, "000000000000000", sizeof(g_device_id) - 1);
        return 0;
    }

    strncpy(imei, modem_param.device.imei.value_string, sizeof(imei) - 1);

    /* Strip trailing whitespace / newline that modem_info sometimes adds */
    for (int i = strlen(imei) - 1; i >= 0; i--) {
        if (imei[i] == '\r' || imei[i] == '\n' || imei[i] == ' ') {
            imei[i] = '\0';
        } else {
            break;
        }
    }

    strncpy(g_device_id, imei, sizeof(g_device_id) - 1);
    LOG_INF("Device ID derived from IMEI: %s", g_device_id);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Reboot counter — NVS-persisted, incremented on every boot                  */
/* ─────────────────────────────────────────────────────────────────────────── */

#include <zephyr/fs/nvs.h>

#define REBOOT_CNT_NVS_ID  2U   /* ID 1 is used by the provisioning Settings */

static struct nvs_fs reboot_nvs;
static uint32_t      g_reboot_cnt = 0;

static void reboot_counter_init(void)
{
    struct flash_pages_info info;
    reboot_nvs.flash_device = FIXED_PARTITION_DEVICE(storage_partition);
    reboot_nvs.offset       = FIXED_PARTITION_OFFSET(storage_partition);

    if (!device_is_ready(reboot_nvs.flash_device)) {
        LOG_WRN("Reboot counter: flash not ready");
        return -1;
    }
    flash_get_page_info_by_offs(reboot_nvs.flash_device, reboot_nvs.offset, &info);
    reboot_nvs.sector_size  = info.size;
    reboot_nvs.sector_count = 2U;

    if (nvs_mount(&reboot_nvs) != 0) {
        LOG_WRN("Reboot counter: NVS mount failed");
        return -1;
    }

    nvs_read(&reboot_nvs, REBOOT_CNT_NVS_ID, &g_reboot_cnt, sizeof(g_reboot_cnt));
    g_reboot_cnt++;
    nvs_write(&reboot_nvs, REBOOT_CNT_NVS_ID, &g_reboot_cnt, sizeof(g_reboot_cnt));
    LOG_INF("Reboot counter: %u", g_reboot_cnt);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Sensor stubs — replace with real driver calls                              */
/* ─────────────────────────────────────────────────────────────────────────── */

static float read_temperature(void)
{
    /* TODO: replace with your sensor driver, e.g.:
     *   struct sensor_value val;
     *   sensor_sample_fetch(dev);
     *   sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &val);
     *   return sensor_value_to_double(&val);
     */
    return 22.5f;
}

static float read_humidity(void)
{
    return 61.0f; /* TODO: replace */
}

static int16_t read_rssi(void)
{
    struct modem_param_info modem_param;
    int ret = modem_info_params_get(&modem_param);
    if (ret) return INT16_MIN;
    return (int16_t)modem_param.network.rsrp.value;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* ISO-8601 timestamp                                                          */
/* ─────────────────────────────────────────────────────────────────────────── */

static void get_iso8601(char *buf, size_t buf_len)
{
    int64_t unix_ms;
    int ret = date_time_now(&unix_ms);
    if (ret) {
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
/* Telemetry publish (normal operation)                                       */
/* ─────────────────────────────────────────────────────────────────────────── */

static int publish_telemetry(void)
{
    char timestamp[32];
    get_iso8601(timestamp, sizeof(timestamp));

    float   temperature = read_temperature();
    float   humidity    = read_humidity();
    int16_t rssi        = read_rssi();

    cJSON *root    = cJSON_CreateObject();
    cJSON *metrics = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "deviceId",  g_device_id);
    cJSON_AddStringToObject(root, "timestamp", timestamp);
    cJSON_AddItemToObject(root, "metrics", metrics);
    cJSON_AddNumberToObject(metrics, "temperature", (double)temperature);
    cJSON_AddNumberToObject(metrics, "humidity",    (double)humidity);
    if (rssi != INT16_MIN) {
        cJSON_AddNumberToObject(metrics, "_rssi", (double)rssi);
    }
    cJSON_AddNumberToObject(metrics, "_reboot_cnt", (double)g_reboot_cnt);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!payload) return -ENOMEM;

    struct mqtt_publish_param msg = {
        .message.topic.qos        = MQTT_QOS_1_AT_LEAST_ONCE,
        .message.topic.topic.utf8 = (uint8_t *)telemetry_topic,
        .message.topic.topic.size = strlen(telemetry_topic),
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
        LOG_INF("Telemetry: temp=%.1f hum=%.1f rssi=%d",
                (double)temperature, (double)humidity, (int)rssi);
    }

    cJSON_FreeString(payload);
    return ret;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* MQTT ACK topics (normal operation)                                         */
/* ─────────────────────────────────────────────────────────────────────────── */
static char cmd_ack_topic[80];   /* devices/<id>/commands/ack */
static char cfg_ack_topic[80];   /* devices/<id>/config/ack  */

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
        .message.topic.topic.utf8 = (uint8_t *)cmd_ack_topic,
        .message.topic.topic.size = strlen(cmd_ack_topic),
        .message.payload.data     = (uint8_t *)json,
        .message.payload.len      = strlen(json),
        .message_id               = (uint16_t)(k_uptime_get_32() & 0xFFFF),
    };
    mqtt_publish(&client, &msg);
    cJSON_FreeString(json);
}

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
        .message.topic.topic.utf8 = (uint8_t *)cfg_ack_topic,
        .message.topic.topic.size = strlen(cfg_ack_topic),
        .message.payload.data     = (uint8_t *)json,
        .message.payload.len      = strlen(json),
        .message_id               = (uint16_t)(k_uptime_get_32() & 0xFFFF),
    };
    mqtt_publish(&client, &msg);
    cJSON_FreeString(json);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Incoming command handler (normal operation)                                */
/* ─────────────────────────────────────────────────────────────────────────── */

static void handle_command(const char *json_str, size_t len)
{
    char buf[512];
    size_t copy_len = MIN(len, sizeof(buf) - 1);
    memcpy(buf, json_str, copy_len);
    buf[copy_len] = '\0';

    cJSON *msg = cJSON_Parse(buf);
    if (!msg) {
        LOG_WRN("Failed to parse incoming JSON");
        return -1;
    }

    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "type"));
    if (!type) {
        cJSON_Delete(msg);
        return -1;
    }

    if (strcmp(type, "config") == 0) {
        const cJSON *config    = cJSON_GetObjectItem(msg, "config");
        const char  *config_id = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "configId"));

        if (config) {
            const cJSON *interval_item = cJSON_GetObjectItem(config, "telemetryIntervalSec");
            if (cJSON_IsNumber(interval_item)) {
                int new_interval = (int)interval_item->valuedouble;
                if (new_interval >= 10 && new_interval <= 3600) {
                    telemetry_interval_sec = new_interval;
                    LOG_INF("Setting: telemetryIntervalSec → %ds", telemetry_interval_sec);
                }
            }
            /* Add more config keys here as needed */
        }

        /* ACK the config push — updates status to 'applied' in OTA Config page */
        publish_config_ack(config_id, true);

    } else if (strcmp(type, "command") == 0) {
        const char *cmd        = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "command"));
        const char *command_id = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "commandId"));
        const char *sk         = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "sk"));
        const cJSON *payload   = cJSON_GetObjectItem(msg, "payload");

        if (!cmd) {
            cJSON_Delete(msg);
            return -1;
        }

        LOG_INF("Command: %s (id=%s source=%s)", cmd,
                command_id ? command_id : "?",
                cJSON_GetStringValue(cJSON_GetObjectItem(msg, "source"))
                    ? cJSON_GetStringValue(cJSON_GetObjectItem(msg, "source")) : "?");

        if (strcmp(cmd, "REBOOT") == 0) {
            publish_command_ack(command_id, sk, "rebooting");
            cJSON_Delete(msg);
            k_sleep(K_MSEC(500));
            sys_reboot(SYS_REBOOT_COLD);
            return -1;

        } else if (strcmp(cmd, "FAN_ON") == 0) {
            const cJSON *speed = payload ? cJSON_GetObjectItem(payload, "speed") : NULL;
            LOG_INF("FAN_ON speed=%d%%", cJSON_IsNumber(speed) ? (int)speed->valuedouble : 100);
            /* TODO: gpio_pin_set(fan_dev, FAN_PIN, 1); */
            publish_command_ack(command_id, sk, "fan_on");

        } else if (strcmp(cmd, "FAN_OFF") == 0) {
            LOG_INF("FAN_OFF");
            /* TODO: gpio_pin_set(fan_dev, FAN_PIN, 0); */
            publish_command_ack(command_id, sk, "fan_off");

        } else if (strcmp(cmd, "SET_INTERVAL") == 0) {
            /*
             * Interval is in SECONDS — consistent with the OTA Config page
             * telemetryIntervalSec setting and all SDK samples.
             * Example: { "interval": 120 } → 2-minute reporting.
             */
            const cJSON *iv = payload ? cJSON_GetObjectItem(payload, "interval") : NULL;
            if (cJSON_IsNumber(iv)) {
                int new_sec = (int)iv->valuedouble;
                if (new_sec >= 10 && new_sec <= 3600) {
                    telemetry_interval_sec = new_sec;
                    LOG_INF("SET_INTERVAL: telemetry interval → %ds", new_sec);
                    publish_command_ack(command_id, sk, "interval_set");
                } else {
                    publish_command_ack(command_id, sk, "out_of_range");
                }
            } else {
                publish_command_ack(command_id, sk, "invalid_payload");
            }

        } else {
            LOG_WRN("Unknown command: %s", cmd);
            publish_command_ack(command_id, sk, "unknown_command");
        }
    }

    cJSON_Delete(msg);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* MQTT event handler — normal operation phase                                */
/* ─────────────────────────────────────────────────────────────────────────── */

static void normal_mqtt_event_handler(struct mqtt_client *c,
                                      const struct mqtt_evt *evt)
{
    int ret;

    switch (evt->type) {

    case MQTT_EVT_CONNACK:
        if (evt->result == 0) {
            LOG_INF("MQTT connected (normal operation) — device: %s", g_device_id);
            mqtt_connected = true;

            /*
             * Subscribe to both topics the cloud publishes to this device:
             *   commands — Commands/Schedules page
             *   config   — OTA Config page real-time pushes
             */
            struct mqtt_topic sub_topics[2] = {
                {
                    .topic = { .utf8 = (uint8_t *)command_topic,
                               .size = strlen(command_topic) },
                    .qos = MQTT_QOS_1_AT_LEAST_ONCE,
                },
                {
                    .topic = { .utf8 = (uint8_t *)config_topic,
                               .size = strlen(config_topic) },
                    .qos = MQTT_QOS_1_AT_LEAST_ONCE,
                },
            };
            const struct mqtt_subscription_list sub_list = {
                .list       = sub_topics,
                .list_count = ARRAY_SIZE(sub_topics),
                .message_id = 1,
            };
            ret = mqtt_subscribe(c, &sub_list);
            if (ret) {
                LOG_ERR("mqtt_subscribe failed (err %d)", ret);
            } else {
                LOG_INF("Subscribed to: %s and %s", command_topic, config_topic);
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
        const struct mqtt_publish_param *p = &evt->param.publish;
        size_t payload_len = MIN(p->message.payload.len, sizeof(mqtt_payload_buf) - 1);

        ret = mqtt_read_publish_payload_blocking(c, mqtt_payload_buf, payload_len);
        if (ret < 0) break;
        mqtt_payload_buf[ret] = '\0';

        handle_command((char *)mqtt_payload_buf, ret);

        if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
            struct mqtt_puback_param puback = { .message_id = p->message_id };
            mqtt_publish_qos1_ack(c, &puback);
        }
        break;
    }

    case MQTT_EVT_PUBACK:
        LOG_DBG("PUBACK (id %d)", evt->param.puback.message_id);
        break;

    case MQTT_EVT_SUBACK:
        LOG_INF("Subscribed to command topic");
        break;

    case MQTT_EVT_PINGRESP:
        break;

    default:
        LOG_DBG("Unhandled MQTT event: %d", evt->type);
        break;
    }
}

/* Forward declaration used by provision.c event handler */
void provision_mqtt_event_handler(struct mqtt_client *client,
                                  const struct mqtt_evt *evt);

/* ─────────────────────────────────────────────────────────────────────────── */
/* Broker address resolution                                                   */
/* ─────────────────────────────────────────────────────────────────────────── */

static int resolve_broker_addr(void)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
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
/* sec_tags selects CLAIM (provisioning) or DEVICE (normal) credentials.     */
/* ─────────────────────────────────────────────────────────────────────────── */

static void mqtt_client_setup(bool use_claim_creds)
{
    mqtt_client_init(&client);

    client.broker        = &broker_addr;
    client.evt_cb        = use_claim_creds
                           ? provision_mqtt_event_handler
                           : normal_mqtt_event_handler;

    client.client_id.utf8 = (uint8_t *)g_device_id;
    client.client_id.size = strlen(g_device_id);

    client.password         = NULL;
    client.user_name        = NULL;
    client.protocol_version = MQTT_VERSION_3_1_1;
    client.keepalive        = CONFIG_CONEXIO_MQTT_KEEPALIVE_SEC;
    client.clean_session    = 1;

    client.rx_buf      = mqtt_rx_buf;
    client.rx_buf_size = sizeof(mqtt_rx_buf);
    client.tx_buf      = mqtt_tx_buf;
    client.tx_buf_size = sizeof(mqtt_tx_buf);

    client.transport.type = MQTT_TRANSPORT_SECURE;
    struct mqtt_sec_config *tls = &client.transport.tls.config;
    tls->peer_verify   = TLS_PEER_VERIFY_REQUIRED;
    tls->cipher_count  = 0;
    tls->cipher_list   = NULL;
    tls->hostname      = AWS_BROKER_HOST;
    tls->session_cache = TLS_SESSION_CACHE_DISABLED;

    if (use_claim_creds) {
        /* Provisioning phase: use shared bootstrap certificate */
        static sec_tag_t claim_tags[] = {
            CONFIG_CONEXIO_CLAIM_CA_TAG,
            CONFIG_CONEXIO_CLAIM_CERT_TAG,
            CONFIG_CONEXIO_CLAIM_KEY_TAG,
        };
        tls->sec_tag_list  = claim_tags;
        tls->sec_tag_count = ARRAY_SIZE(claim_tags);
        LOG_INF("MQTT client configured with CLAIM credentials (tags %d/%d/%d)",
                CONFIG_CONEXIO_CLAIM_CA_TAG,
                CONFIG_CONEXIO_CLAIM_CERT_TAG,
                CONFIG_CONEXIO_CLAIM_KEY_TAG);
    } else {
        /* Normal operation: use unique device certificate */
        static sec_tag_t device_tags[] = {
            CONFIG_CONEXIO_DEVICE_CA_TAG,
            CONFIG_CONEXIO_DEVICE_CERT_TAG,
            CONFIG_CONEXIO_DEVICE_KEY_TAG,
        };
        tls->sec_tag_list  = device_tags;
        tls->sec_tag_count = ARRAY_SIZE(device_tags);
        LOG_INF("MQTT client configured with DEVICE credentials (tags %d/%d/%d)",
                CONFIG_CONEXIO_DEVICE_CA_TAG,
                CONFIG_CONEXIO_DEVICE_CERT_TAG,
                CONFIG_CONEXIO_DEVICE_KEY_TAG);
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* LTE                                                                         */
/* ─────────────────────────────────────────────────────────────────────────── */

static K_SEM_DEFINE(lte_ready, 0, 1);

static void lte_event_handler(const struct lte_lc_evt *const evt)
{
    if (evt->type == LTE_LC_EVT_NW_REG_STATUS) {
        if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
            evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
            LOG_INF("LTE registered (%s)",
                    evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME
                    ? "home" : "roaming");
            k_sem_give(&lte_ready);
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Main                                                                        */
/* ─────────────────────────────────────────────────────────────────────────── */

int main(void)
{
    int ret;

    LOG_INF("=== Conexio Console Fleet Provisioning sample ===");
    LOG_INF("Broker: %s:%d", AWS_BROKER_HOST, AWS_BROKER_PORT);

    /* 1. Load provisioning flag from flash */
    ret = settings_subsys_init();
    if (ret) {
        LOG_WRN("settings_subsys_init failed (err %d) — continuing without persistence", ret);
    } else {
        settings_load_subtree("prov");
    }

    /* 2. Provision claim credentials into modem (safe to call every boot) */
    ret = cert_store_provision_claim_creds();
    if (ret) {
        LOG_ERR("Failed to provision claim creds (err %d) — halting", ret);
        return -1;
    }

    /* 3. Init modem info (IMEI + RSSI) */
    modem_info_init();

    /* 4. Initialise reboot counter (NVS) */
    reboot_counter_init();

    /* 4. Connect to LTE
     * NCS v3.2.1: lte_lc_init_and_connect_async() removed — use lte_lc_connect_async() */
    LOG_INF("Connecting to LTE...");
    ret = lte_lc_connect_async(lte_event_handler);
    if (ret) {
        LOG_ERR("lte_lc_connect_async failed (err %d)", ret);
        return -1;
    }
    ret = k_sem_take(&lte_ready, K_SECONDS(90));
    if (ret) {
        LOG_ERR("LTE registration timed out");
        return -1;
    }

    /* 5. Sync time via NTP */
    date_time_update_async(NULL);
    k_sleep(K_SECONDS(3));

    /* 6. Derive device ID from IMEI */
    ret = derive_device_id();
    if (ret) {
        LOG_ERR("Failed to derive device ID — halting");
        return -1;
    }

    /* 7. Resolve broker once (same host for both phases) */
    ret = resolve_broker_addr();
    if (ret) {
        return -1;
    }

    /* ── PHASE A: USB/Serial commissioning (dashboard WebSerial flow) ──────── */
    /*                                                                          */
    /* If the device cert doesn't exist yet AND a serial port is connected,    */
    /* wait up to 30 seconds for the dashboard to push credentials over UART.  */
    /* If nothing arrives, fall through to Fleet Provisioning (Phase B).       */

    bool already_provisioned = g_provisioning_done ||
                               cert_store_device_creds_exist();

    if (!already_provisioned) {
        LOG_INF("Starting UART commissioning listener (30s window)...");
        LOG_INF("Connect the device to a USB port and use the dashboard");
        LOG_INF("Provisioning → USB / Serial tab to push credentials.");

        ret = uart_commissioning_init();
        if (ret == 0) {
            int64_t uart_deadline = k_uptime_get() + 30000;
            bool uart_done = false;

            while (k_uptime_get() < uart_deadline && !uart_done) {
                uart_comm_status_t status =
                    uart_commissioning_poll(g_device_id, CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION);

                if (status == UART_COMM_PROVISIONED) {
                    LOG_INF("Device commissioned via USB/Serial — rebooting");
                    persist_provisioning_done();
                    k_sleep(K_SECONDS(1));
                    sys_reboot(SYS_REBOOT_COLD);
                    return; /* unreachable */
                }
                k_sleep(K_MSEC(50));
            }

            LOG_INF("UART commissioning window elapsed — proceeding to Fleet Provisioning");
        } else {
            LOG_WRN("UART commissioning init failed (%d) — skipping", ret);
        }
    }

    /* ── PHASE B: Fleet Provisioning (first boot, no USB connected) ────────── */

    if (!already_provisioned) {
        LOG_INF("Device not yet provisioned — starting Fleet Provisioning...");

        /* Set up MQTT client with claim credentials for provisioning session */
        mqtt_client_setup(true /* use_claim_creds */);

        ret = run_provisioning(g_device_id, &client);
        if (ret) {
            LOG_ERR("Fleet Provisioning failed (err %d)", ret);
            LOG_ERR("Check AWS IoT Console → Fleet Provisioning → Templates");
            LOG_ERR("Check that the claim cert policy allows iot:CreateKeysAndCertificate");
            /* Retry after a delay rather than halting permanently */
            k_sleep(K_SECONDS(30));
            sys_reboot(SYS_REBOOT_COLD);
            return -1;
        }

        /* Persist flag so we skip provisioning on subsequent boots */
        persist_provisioning_done();

        LOG_INF("Provisioning complete — rebooting to start normal operation");
        k_sleep(K_SECONDS(2)); /* flush logs */
        sys_reboot(SYS_REBOOT_COLD);
        return; /* unreachable */
    }

    /* ── PHASE B: Normal operation ──────────────────────────────────────── */

    LOG_INF("Device already provisioned — starting normal operation");

    /* Build MQTT topics using the device ID */
    snprintf(telemetry_topic, sizeof(telemetry_topic),
             "devices/%s/telemetry", g_device_id);
    snprintf(command_topic, sizeof(command_topic),
             "devices/%s/commands", g_device_id);
    snprintf(config_topic, sizeof(config_topic),
             "devices/%s/config", g_device_id);
    snprintf(cmd_ack_topic, sizeof(cmd_ack_topic),
             "devices/%s/commands/ack", g_device_id);
    snprintf(cfg_ack_topic, sizeof(cfg_ack_topic),
             "devices/%s/config/ack", g_device_id);

    LOG_INF("Telemetry topic: %s", telemetry_topic);
    LOG_INF("Command topic  : %s", command_topic);
    LOG_INF("Telemetry interval: %ds", telemetry_interval_sec);

    /* Set up MQTT client with unique device credentials */
    mqtt_client_setup(false /* use_device_creds */);

    /* ── Main loop ──────────────────────────────────────────────────────── */
    while (1) {
        if (!mqtt_connected) {
            LOG_INF("Connecting to AWS IoT Core...");
            ret = mqtt_connect(&client);
            if (ret) {
                LOG_ERR("mqtt_connect failed (err %d) — retrying in 10s", ret);
                k_sleep(K_SECONDS(10));
                continue;
            }
        }

        /* Drive MQTT */
        struct zsock_pollfd fds = {
            .fd     = client.transport.tls.sock,
            .events = ZSOCK_POLLIN,
        };
        ret = zsock_poll(&fds, 1, K_SECONDS(1).ticks);
        if (ret > 0 && (fds.revents & ZSOCK_POLLIN)) {
            ret = mqtt_input(&client);
            if (ret) {
                LOG_WRN("mqtt_input error (%d)", ret);
                mqtt_disconnect(&client);
                continue;
            }
        }

        ret = mqtt_live(&client);
        if (ret && ret != -EAGAIN) {
            LOG_WRN("mqtt_live error (%d)", ret);
            mqtt_disconnect(&client);
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
