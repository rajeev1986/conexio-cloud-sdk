/*
 * main.c — Conexio Stratus AWS Fleet Provisioning
 * NCS v3.2.1 / nRF9151
 *
 * Boot flow:
 *   First boot  → provision claim creds → LTE → Fleet Provisioning → reboot
 *   Subsequent  → LTE → MQTT (device creds) → telemetry + commands
 */

#include <zephyr/kernel.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/settings/settings.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include <modem/modem_info.h>

#include <date_time.h>
#include <cJSON.h>
#include <cJSON_os.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cert_store.h"
#include "provision.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* ── Device ID ───────────────────────────────────────────────────────────── */
#define DEVICE_ID_LEN 32
static char g_device_id[DEVICE_ID_LEN];

/* ── Provisioning state ──────────────────────────────────────────────────── */
#define PROV_SETTINGS_KEY "prov/done"
static bool g_provisioned;

/* ── Reboot counter (NVS) ────────────────────────────────────────────────── */
#define NVS_REBOOT_ID 1U  /* unused — reboot counter now stored via Settings */
static struct nvs_fs reboot_nvs;  /* unused — kept to avoid removing includes */
static uint32_t g_reboot_cnt;

/* ── MQTT ────────────────────────────────────────────────────────────────── */
/* rx/tx buffers must be 4-byte aligned for the nRF91xx modem IPC.
 * Use __aligned(4) to guarantee this regardless of placement. */
#define MQTT_RX_BUF_SIZE 2048
#define MQTT_TX_BUF_SIZE 1500

static uint8_t mqtt_rx_buf[MQTT_RX_BUF_SIZE] __aligned(4);
static uint8_t mqtt_tx_buf[MQTT_TX_BUF_SIZE] __aligned(4);
static struct mqtt_client client;
static struct sockaddr_storage broker_addr __aligned(4);
static bool mqtt_connected;

/* ── Telemetry ───────────────────────────────────────────────────────────── */
static char topic_telemetry[64];
static char topic_command[64];
static int  telemetry_interval_sec = CONFIG_STRATUS_TELEMETRY_INTERVAL_SEC;

/* ── LTE ─────────────────────────────────────────────────────────────────── */
static K_SEM_DEFINE(lte_ready, 0, 1);

static void lte_handler(const struct lte_lc_evt *const evt)
{
    if (evt->type == LTE_LC_EVT_NW_REG_STATUS &&
        (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
         evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
        LOG_INF("LTE registered (%s)",
                evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME
                ? "home" : "roaming");
        k_sem_give(&lte_ready);
    }
}

/* ── Settings ────────────────────────────────────────────────────────────── */

static int settings_h_set(const char *key, size_t len,
                           settings_read_cb read_cb, void *cb_arg)
{
    LOG_DBG("settings_h_set key='%s' len=%u", key, (unsigned)len);
    if (strcmp(key, "done") == 0) {
        bool val = false;
        ssize_t rc = read_cb(cb_arg, &val, sizeof(val));
        LOG_INF("settings_h_set 'done': read_cb rc=%d val=%d",
                (int)rc, (int)val);
        if (rc > 0) {
            g_provisioned = val;
        }
    } else if (strcmp(key, "reboot_cnt") == 0) {
        read_cb(cb_arg, &g_reboot_cnt, sizeof(g_reboot_cnt));
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(prov_handler, "prov", NULL,
                                settings_h_set, NULL, NULL);

static void persist_provisioned(void)
{
    bool val = true;
    int ret = settings_save_one(PROV_SETTINGS_KEY, &val, sizeof(val));
    if (ret) {
        LOG_WRN("settings_save_one failed: %d", ret);
    } else {
        LOG_INF("Provisioning flag saved to flash");
    }
}

/* ── Reboot counter (via Settings NVS — same instance as provisioning flag) ── */
#define REBOOT_CNT_KEY "prov/reboot_cnt"

static void reboot_counter_init(void)
{
    /* settings_subsys_init() was already called before this function.
     * Read current count, increment, and save back — all via Settings NVS.
     * This avoids mounting a second nvs_fs on storage_partition which would
     * conflict with the Settings NVS backend and lose the provisioning flag. */
    int ret = settings_load_subtree("prov/reboot_cnt");

    /* Load may return -ENOENT on first boot — that's fine, counter starts at 0 */
    (void)ret;

    g_reboot_cnt++;

    ret = settings_save_one(REBOOT_CNT_KEY, &g_reboot_cnt, sizeof(g_reboot_cnt));
    if (ret) {
        LOG_WRN("Reboot counter save failed: %d", ret);
    }
    LOG_INF("Reboot counter: %u", g_reboot_cnt);
}

/* ── IMEI → device ID ────────────────────────────────────────────────────── */

static int derive_device_id(void)
{
    /* modem_info_string_get(MODEM_INFO_IMEI) issues AT+CGSN directly.
     * Works before LTE registration — no network needed. */
    char imei[MODEM_INFO_MAX_RESPONSE_SIZE] = {0};
    int ret = modem_info_string_get(MODEM_INFO_IMEI, imei, sizeof(imei));
    if (ret < 0) {
        LOG_ERR("modem_info_string_get(IMEI) failed: %d", ret);
        return ret;
    }

    /* Strip trailing CR/LF/space */
    for (int i = (int)strlen(imei) - 1; i >= 0; i--) {
        if (imei[i] == '\r' || imei[i] == '\n' || imei[i] == ' ') {
            imei[i] = '\0';
        } else {
            break;
        }
    }

    if (strlen(imei) == 0) {
        LOG_ERR("IMEI is empty");
        return -ENODATA;
    }

    strncpy(g_device_id, imei, sizeof(g_device_id) - 1);
    LOG_INF("Device ID: %s", g_device_id);
    return 0;
}

/* ── Broker DNS resolution ───────────────────────────────────────────────── */

static int resolve_broker(void)
{
    /* Use zsock_ prefixed APIs directly — NET_SOCKETS_POSIX_NAMES was
     * removed in NCS v2.x so the unprefixed aliases are not available. */
    struct zsock_addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct zsock_addrinfo *res;
    char port[6];

    snprintf(port, sizeof(port), "%d", 8883);

    int ret = zsock_getaddrinfo(CONFIG_STRATUS_AWS_ENDPOINT, port, &hints, &res);
    if (ret) {
        LOG_ERR("zsock_getaddrinfo(%s) failed: %d",
                CONFIG_STRATUS_AWS_ENDPOINT, ret);
        return -ENOENT;
    }

    memcpy(&broker_addr, res->ai_addr, res->ai_addrlen);
    zsock_freeaddrinfo(res);
    LOG_INF("Broker resolved: %s", CONFIG_STRATUS_AWS_ENDPOINT);
    return 0;
}

/* ── Normal operation MQTT event handler — forward declaration ───────────── */
static void normal_mqtt_evt_handler(struct mqtt_client *c,
                                    const struct mqtt_evt *evt);

/* ── MQTT client setup ───────────────────────────────────────────────────── */

static void mqtt_client_setup(bool use_claim)
{
    mqtt_client_init(&client);

    /* mqtt_client_init zeros the struct, leaving tls.sock = 0.
     * fd 0 is a real descriptor (UART) on Zephyr — must set to -1
     * so the MQTT library knows no socket is open yet. */
    client.transport.tls.sock = -1;

    client.broker             = &broker_addr;    client.evt_cb             = use_claim ? provision_mqtt_evt_handler
                                          : normal_mqtt_evt_handler;
    client.client_id.utf8     = (uint8_t *)g_device_id;
    client.client_id.size     = strlen(g_device_id);
    client.password           = NULL;
    client.user_name          = NULL;
    client.protocol_version   = MQTT_VERSION_3_1_1;
    client.keepalive          = CONFIG_STRATUS_MQTT_KEEPALIVE;
    client.clean_session      = 1;
    client.rx_buf             = mqtt_rx_buf;
    client.rx_buf_size        = sizeof(mqtt_rx_buf);
    client.tx_buf             = mqtt_tx_buf;
    client.tx_buf_size        = sizeof(mqtt_tx_buf);

    client.transport.type     = MQTT_TRANSPORT_SECURE;
    struct mqtt_sec_config *tls = &client.transport.tls.config;
    tls->peer_verify            = TLS_PEER_VERIFY_REQUIRED;
    tls->cipher_count           = 0;
    tls->cipher_list            = NULL;
    tls->hostname               = CONFIG_STRATUS_AWS_ENDPOINT;
    tls->session_cache          = TLS_SESSION_CACHE_DISABLED;

    if (use_claim) {
        static sec_tag_t claim_tags[] = {
            CONFIG_STRATUS_CLAIM_CA_TAG,
            CONFIG_STRATUS_CLAIM_CERT_TAG,
            CONFIG_STRATUS_CLAIM_KEY_TAG,
        };
        tls->sec_tag_list  = claim_tags;
        tls->sec_tag_count = ARRAY_SIZE(claim_tags);
        LOG_INF("MQTT: claim creds (tags %d/%d/%d)",
                CONFIG_STRATUS_CLAIM_CA_TAG,
                CONFIG_STRATUS_CLAIM_CERT_TAG,
                CONFIG_STRATUS_CLAIM_KEY_TAG);
    } else {        static sec_tag_t device_tags[] = {
            CONFIG_STRATUS_DEVICE_CA_TAG,
            CONFIG_STRATUS_DEVICE_CERT_TAG,
            CONFIG_STRATUS_DEVICE_KEY_TAG,
        };
        tls->sec_tag_list  = device_tags;
        tls->sec_tag_count = ARRAY_SIZE(device_tags);
        LOG_INF("MQTT: device creds (tags %d/%d/%d)",
                CONFIG_STRATUS_DEVICE_CA_TAG,
                CONFIG_STRATUS_DEVICE_CERT_TAG,
                CONFIG_STRATUS_DEVICE_KEY_TAG);
    }
}

/* ── Telemetry helpers ───────────────────────────────────────────────────── */

/* ISO-8601 timestamp — direct digit write, no snprintf, no -Wformat-truncation */
#define TS_LEN 25  /* "YYYY-MM-DDTHH:MM:SS.mmmZ\0" */

static void make_timestamp(char buf[TS_LEN])
{
    int64_t unix_ms = 0;
    int ret = date_time_now(&unix_ms);

    if (ret || unix_ms <= 0) {
        /* Fallback: uptime as HH:MM:SS — direct digit write, no snprintf */
        uint32_t s  = (uint32_t)(k_uptime_get() / 1000U);
        uint32_t hh = (s / 3600U) % 100U; /* cap at 99 to guarantee 2 digits */
        uint32_t mm = (s % 3600U) / 60U;
        uint32_t ss = s % 60U;
        char *p = buf;
        *p++ = '1'; *p++ = '9'; *p++ = '7'; *p++ = '0'; *p++ = '-';
        *p++ = '0'; *p++ = '1'; *p++ = '-'; *p++ = '0'; *p++ = '1'; *p++ = 'T';
        *p++ = '0' + hh / 10; *p++ = '0' + hh % 10; *p++ = ':';
        *p++ = '0' + mm / 10; *p++ = '0' + mm % 10; *p++ = ':';
        *p++ = '0' + ss / 10; *p++ = '0' + ss % 10;
        *p++ = '.'; *p++ = '0'; *p++ = '0'; *p++ = '0';
        *p++ = 'Z'; *p = '\0';
        return;
    }

    time_t unix_sec = (time_t)(unix_ms / 1000);
    struct tm tm_buf;
    struct tm *t = gmtime_r(&unix_sec, &tm_buf);
    unsigned ms = (unsigned)(unix_ms % 1000);
    int year = t->tm_year + 1900;
    if (year < 1970) year = 1970;
    if (year > 9999) year = 9999;

    char *p = buf;
    *p++ = '0' + (year / 1000) % 10;
    *p++ = '0' + (year /  100) % 10;
    *p++ = '0' + (year /   10) % 10;
    *p++ = '0' + (year       ) % 10;
    *p++ = '-';
    *p++ = '0' + ((t->tm_mon + 1) / 10) % 10;
    *p++ = '0' + ((t->tm_mon + 1)     ) % 10;
    *p++ = '-';
    *p++ = '0' + (t->tm_mday / 10) % 10;
    *p++ = '0' + (t->tm_mday     ) % 10;
    *p++ = 'T';
    *p++ = '0' + (t->tm_hour / 10) % 10;
    *p++ = '0' + (t->tm_hour     ) % 10;
    *p++ = ':';
    *p++ = '0' + (t->tm_min / 10) % 10;
    *p++ = '0' + (t->tm_min     ) % 10;
    *p++ = ':';
    *p++ = '0' + (t->tm_sec / 10) % 10;
    *p++ = '0' + (t->tm_sec     ) % 10;
    *p++ = '.';
    *p++ = '0' + (ms / 100) % 10;
    *p++ = '0' + (ms /  10) % 10;
    *p++ = '0' + (ms      ) % 10;
    *p++ = 'Z';
    *p   = '\0';
}

static int16_t read_rssi(void)
{
    struct modem_param_info mp;
    if (modem_info_params_get(&mp)) return INT16_MIN;
    return (int16_t)mp.network.rsrp.value;
}

static int publish_telemetry(void)
{
    char ts[TS_LEN];
    make_timestamp(ts);

    int16_t rssi = read_rssi();

    cJSON *root    = cJSON_CreateObject();
    cJSON *metrics = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "deviceId",  g_device_id);
    cJSON_AddStringToObject(root, "timestamp", ts);
    cJSON_AddItemToObject(root, "metrics", metrics);
    cJSON_AddNumberToObject(metrics, "temperature", 22.5);   /* replace with real sensor */
    cJSON_AddNumberToObject(metrics, "humidity",    61.0);   /* replace with real sensor */
    if (rssi != INT16_MIN) {
        cJSON_AddNumberToObject(metrics, "_rssi", (double)rssi);
    }
    cJSON_AddNumberToObject(metrics, "_reboot_cnt", (double)g_reboot_cnt);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return -ENOMEM;

    struct mqtt_publish_param msg = {
        .message.topic.qos        = MQTT_QOS_1_AT_LEAST_ONCE,
        .message.topic.topic.utf8 = (uint8_t *)topic_telemetry,
        .message.topic.topic.size = strlen(topic_telemetry),
        .message.payload.data     = (uint8_t *)payload,
        .message.payload.len      = strlen(payload),
        .message_id               = (uint16_t)(k_uptime_get_32() & 0xFFFF),
    };

    int ret = mqtt_publish(&client, &msg);
    if (ret) {
        LOG_ERR("mqtt_publish telemetry failed: %d", ret);
    } else {
        LOG_INF("Telemetry published: %s", payload);
    }

    cJSON_free(payload);
    return ret;
}

/* ── Normal operation MQTT event handler ─────────────────────────────────── */

static uint8_t cmd_payload_buf[512];

static void normal_mqtt_evt_handler(struct mqtt_client *c,
                                    const struct mqtt_evt *evt)
{
    switch (evt->type) {

    case MQTT_EVT_CONNACK:
        if (evt->result == 0) {
            LOG_INF("MQTT connected — device: %s", g_device_id);
            mqtt_connected = true;

            struct mqtt_topic sub = {
                .topic = { .utf8 = (uint8_t *)topic_command,
                           .size = strlen(topic_command) },
                .qos = MQTT_QOS_1_AT_LEAST_ONCE,
            };
            const struct mqtt_subscription_list sl = {
                .list = &sub, .list_count = 1, .message_id = 1,
            };
            if (mqtt_subscribe(c, &sl)) {
                LOG_WRN("Subscribe to command topic failed");
            } else {
                LOG_INF("Subscribed: %s", topic_command);
            }
        } else {
            LOG_ERR("CONNACK error %d", evt->result);
            mqtt_connected = false;
        }
        break;

    case MQTT_EVT_DISCONNECT:
        LOG_WRN("MQTT disconnected: %d", evt->result);
        mqtt_connected = false;
        break;

    case MQTT_EVT_PUBLISH: {
        const struct mqtt_publish_param *p = &evt->param.publish;
        size_t plen = MIN(p->message.payload.len, sizeof(cmd_payload_buf) - 1);

        if (mqtt_readall_publish_payload(c, cmd_payload_buf, plen) == 0) {
            cmd_payload_buf[plen] = '\0';
            LOG_INF("Command received: %s", cmd_payload_buf);

            /* Parse and dispatch commands */
            cJSON *msg = cJSON_Parse((char *)cmd_payload_buf);
            if (msg) {
                const char *cmd = cJSON_GetStringValue(
                    cJSON_GetObjectItem(msg, "command"));
                if (cmd && strcmp(cmd, "REBOOT") == 0) {
                    LOG_INF("REBOOT command — rebooting in 1s");
                    k_sleep(K_SECONDS(1));
                    sys_reboot(SYS_REBOOT_COLD);
                } else if (cmd && strcmp(cmd, "SET_INTERVAL") == 0) {
                    const cJSON *iv = cJSON_GetObjectItem(
                        cJSON_GetObjectItem(msg, "payload"), "interval");
                    if (cJSON_IsNumber(iv)) {
                        int sec = (int)iv->valuedouble;
                        if (sec >= 10 && sec <= 3600) {
                            telemetry_interval_sec = sec;
                            LOG_INF("Telemetry interval: %ds", sec);
                        }
                    }
                }
                cJSON_Delete(msg);
            }
        }

        if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
            struct mqtt_puback_param ack = { .message_id = p->message_id };
            mqtt_publish_qos1_ack(c, &ack);
        }
        break;
    }

    case MQTT_EVT_PUBACK:
        LOG_DBG("PUBACK id=%d", evt->param.puback.message_id);
        break;

    case MQTT_EVT_SUBACK:
        LOG_INF("SUBACK — subscribed to command topic");
        break;

    case MQTT_EVT_PINGRESP:
        break;

    default:
        break;
    }
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    int ret;

    LOG_INF("=== Conexio Stratus Fleet Provisioning ===");
    LOG_INF("Endpoint: %s:8883", CONFIG_STRATUS_AWS_ENDPOINT);

    /* 1. Load persisted provisioning flag */
    ret = settings_subsys_init();
    if (ret) {
        LOG_WRN("settings_subsys_init failed: %d — continuing", ret);
    } else {
        ret = settings_load_subtree("prov");
        LOG_INF("settings_load_subtree(prov) ret=%d g_provisioned=%d g_reboot_cnt=%u",
                ret, (int)g_provisioned, (unsigned)g_reboot_cnt);
    }

    /* 2. Init modem library — must be done before any modem_key_mgmt call.
     *    The modem starts in offline (AT-command) mode here. */
    ret = nrf_modem_lib_init();
    if (ret) {
        LOG_ERR("nrf_modem_lib_init failed: %d", ret);
        return -1;
    }

    /* Keep modem offline while writing credentials */
    ret = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE);
    if (ret) {
        LOG_WRN("lte_lc_func_mode_set(OFFLINE) failed: %d — continuing", ret);
    }

    /* 3. Provision shared claim credentials (skip if already present) */
    ret = cert_store_provision_claim_creds();
    if (ret) {
        LOG_ERR("cert_store_provision_claim_creds failed: %d", ret);
        return -1;
    }

    /* 4. Read IMEI — AT+CGSN works in offline mode */
    modem_info_init();
    ret = derive_device_id();
    if (ret) {
        LOG_ERR("derive_device_id failed: %d", ret);
        return -1;
    }

    /* 5. Reboot counter */
    reboot_counter_init();

    /* 5a. Generate device key pair + CSR while modem is still offline.
     *     AT%KEYGEN requires LTE to be inactive.
     *     IMPORTANT: only run on unprovisioned devices. On already-provisioned
     *     devices the private key at tag 21 must NOT be overwritten — it would
     *     invalidate the certificate that AWS issued for the previous key pair.
     *     Use g_provisioned (settings flag) + cert_store_device_creds_exist()
     *     to detect the provisioned state before LTE connects. */
    bool creds_exist = cert_store_device_creds_exist();
    LOG_INF("Boot check: g_provisioned=%d cert_exists=%d",
            (int)g_provisioned, (int)creds_exist);
    bool pre_check_provisioned = g_provisioned || creds_exist;
    if (!pre_check_provisioned) {
        ret = provision_prepare_csr();
        if (ret) {
            LOG_ERR("provision_prepare_csr failed: %d", ret);
            return -1;
        }
    } else {
        LOG_INF("Already provisioned — skipping AT%%KEYGEN");
    }

    /* 6. Connect to LTE */
    LOG_INF("Connecting to LTE...");
    ret = lte_lc_connect_async(lte_handler);
    if (ret) {
        LOG_ERR("lte_lc_connect_async failed: %d", ret);
        return -1;
    }
    if (k_sem_take(&lte_ready, K_SECONDS(120))) {
        LOG_ERR("LTE registration timed out — rebooting to retry");
        lte_lc_power_off();
        k_sleep(K_MSEC(500));
        sys_reboot(SYS_REBOOT_COLD);
        return -1;
    }

    /* 7. Sync NTP */
    date_time_update_async(NULL);
    k_sleep(K_SECONDS(3));

    /* 8. Resolve broker DNS */
    ret = resolve_broker();
    if (ret) {
        return -1;
    }

    /* ── PROVISIONING PATH ───────────────────────────────────────────────── */
    bool already_provisioned = g_provisioned || creds_exist;

    if (!already_provisioned) {
        LOG_INF("Device not provisioned — starting Fleet Provisioning");

        /* CSR was already generated during the offline window at boot
         * (provision_prepare_csr was called before LTE connected).
         * Clear only the stale public cert — private key was regenerated.
         * Settings flag is cleared for consistency. */
        cert_store_clear_device_creds();
        settings_delete(PROV_SETTINGS_KEY);
        g_provisioned = false;

        mqtt_client_setup(true /* claim creds */);

        ret = run_provisioning(g_device_id, &client);
        if (ret) {
            LOG_ERR("Fleet Provisioning failed: %d — retrying in 30s", ret);
            k_sleep(K_SECONDS(30));
            sys_reboot(SYS_REBOOT_COLD);
            return -1;
        }

        persist_provisioned();
        /* Force-flush all pending NVS writes before the reboot. */
        settings_save();
        LOG_INF("Provisioning complete -- shutting down modem and rebooting");

        /* CRITICAL: use nrf_modem_lib_shutdown() to properly deinitialize
         * the modem before rebooting. This ensures the modem has flushed its
         * NVM (including the newly written device certificate at tag 21)
         * before the application core resets.
         *
         * lte_lc_power_off() alone (AT+CFUN=0) is not sufficient — the app
         * core may reboot before the modem completes its NVM write cycle.
         * nrf_modem_lib_shutdown() sends AT+CFUN=0, waits for the modem to
         * acknowledge shutdown, then closes the IPC channel cleanly. */
        lte_lc_power_off();
        k_sleep(K_MSEC(1000));  /* give modem time to flush NVM after CFUN=0 */
        nrf_modem_lib_shutdown();
        k_sleep(K_MSEC(500));   /* wait for IPC shutdown to complete */
        sys_reboot(SYS_REBOOT_COLD);
        return 0;
    }

    /* ── NORMAL OPERATION ────────────────────────────────────────────────── */
    LOG_INF("Device provisioned — starting normal operation");

    snprintf(topic_telemetry, sizeof(topic_telemetry),
             "devices/%s/telemetry", g_device_id);
    snprintf(topic_command, sizeof(topic_command),
             "devices/%s/commands", g_device_id);
    LOG_INF("Telemetry topic: %s", topic_telemetry);
    LOG_INF("Command topic:   %s", topic_command);

    mqtt_client_setup(false /* device creds */);

    /* ── Main loop ───────────────────────────────────────────────────────── */
    /* Use zsock_pollfd — NET_SOCKETS_POSIX_NAMES removed in NCS v2.x.
     * zsock_poll() timeout is in milliseconds; mqtt_keepalive_time_left()
     * returns ms until next PINGREQ — using it drives keepalives precisely
     * without busy-waiting. */
    struct zsock_pollfd fds = {
        .events = ZSOCK_POLLIN,
    };

    int64_t last_pub_ms = 0;

    while (1) {
        if (!mqtt_connected) {
            LOG_INF("Connecting to AWS IoT...");
            ret = mqtt_connect(&client);
            if (ret) {
                LOG_ERR("mqtt_connect failed: %d — retrying in 10s", ret);
                k_sleep(K_SECONDS(10));
                continue;
            }
            fds.fd = client.transport.tls.sock;
        }

        ret = zsock_poll(&fds, 1, mqtt_keepalive_time_left(&client));
        if (ret < 0) {
            LOG_ERR("zsock_poll error: %d", ret);
            mqtt_disconnect(&client, NULL);
            mqtt_connected = false;
            continue;
        }

        /* Drive keepalive (PINGREQ) */
        ret = mqtt_live(&client);
        if (ret && ret != -EAGAIN) {
            LOG_WRN("mqtt_live error: %d", ret);
            mqtt_disconnect(&client, NULL);
            mqtt_connected = false;
            continue;
        }

        if (fds.revents & ZSOCK_POLLIN) {
            ret = mqtt_input(&client);
            if (ret) {
                LOG_WRN("mqtt_input error: %d", ret);
                mqtt_disconnect(&client, NULL);
                mqtt_connected = false;
                continue;
            }
        }

        if (fds.revents & (ZSOCK_POLLERR | ZSOCK_POLLNVAL | ZSOCK_POLLHUP)) {
            LOG_WRN("poll condition: revents=0x%x", fds.revents);
            mqtt_disconnect(&client, NULL);
            mqtt_connected = false;
            continue;
        }

        /* Publish telemetry on schedule */
        int64_t now = k_uptime_get();
        if (mqtt_connected &&
            (now - last_pub_ms >= (int64_t)telemetry_interval_sec * 1000)) {
            publish_telemetry();
            last_pub_ms = now;
        }
    }
}
