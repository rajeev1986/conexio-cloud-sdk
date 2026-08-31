/*
 * mqtt_transport.c — MQTT/TLS transport backend (Phase 2)
 *
 * Implements the internal transport interface (transport.h) for MQTT.
 *
 * Phase 2 difference from Phase 1
 * ─────────────────────────────────
 * In Phase 1 the broker hostname is read from Kconfig (prj.conf).
 * In Phase 2 the hostname comes from config_fetch() at runtime — nothing
 * endpoint-specific is in prj.conf.  The init function is therefore
 * transport_init_with_config() instead of transport_init().
 *
 * MQTT topology
 * ─────────────
 *   Publish  (device → cloud):  devices/<deviceId>/telemetry   QoS 1
 *   Subscribe (cloud → device): devices/<deviceId>/commands    QoS 1
 *                                devices/<deviceId>/config      QoS 1
 *   Publish  (device → cloud):  devices/<deviceId>/commands/ack QoS 1
 *                                devices/<deviceId>/config/ack   QoS 1
 *
 * TLS credentials
 * ───────────────
 *   The modem uses three security tags (Kconfig defaults 100/101/102):
 *     CA_TAG   (100) — AWS Root CA     (written by cert_store.c)
 *     CERT_TAG (101) — Device cert     (written by fleet-provisioning)
 *     KEY_TAG  (102) — Device key      (written by fleet-provisioning)
 *
 * Thread safety
 * ─────────────
 * transport_publish() and transport_poll() are called from the SDK
 * background thread (conexio_cloud.c cloud_thread_fn).  All other
 * functions are called during init, single-threaded.
 */

#include <zephyr/kernel.h>
#include <zephyr/net/mqtt.h>             /* Zephyr MQTT client             */
#include <zephyr/net/socket.h>           /* BSD socket API                 */
#include <zephyr/net/tls_credentials.h>  /* sec_tag_t, TLS_SEC_TAG_LIST   */
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include <cJSON.h>
#include <cJSON_os.h>
#include "../transport.h"
#include "../config_fetch.h"

LOG_MODULE_REGISTER(mqtt_transport, LOG_LEVEL_DBG);

/* AWS IoT Core uses port 8883 for MQTT over TLS (not 1883) */
#define BROKER_PORT 8883

/*
 * Security tags loaded by the modem when establishing the TLS session.
 * The order matters: CA first, then client cert, then private key.
 * These must match the tags written by cert_store.c and fleet-provisioning.
 */
static const sec_tag_t sec_tags[] = {
    CONFIG_CONEXIO_CLOUD_CA_TAG,    /* 100 — AWS Root CA         */
    CONFIG_CONEXIO_CLOUD_CERT_TAG,  /* 101 — Device certificate  */
    CONFIG_CONEXIO_CLOUD_KEY_TAG,   /* 102 — Device private key  */
};

/* ── Module-level state ───────────────────────────────────────────────────*/

static struct mqtt_client     client;
static struct sockaddr_storage broker_addr;

static uint8_t rx_buf[4096];  /* Must fit largest inbound message — FIRMWARE_UPDATE
                                * payload contains a pre-signed S3 URL (~1700 chars)
                                * plus JSON envelope, totalling ~2KB + MQTT framing. */
static uint8_t tx_buf[4096];  /* Must fit largest outbound publish — telemetry JSON
                                * with all metrics + offline buffer replay payloads. */
static uint8_t payload_buf[4096]; /* increased from 2048 — FOTA presigned URLs exceed 2KB */

static bool connected = false;

/* Semaphore signalled when CONNACK arrives — lets transport_connect() block
 * until the connection is established rather than returning immediately. */
static K_SEM_DEFINE(connack_sem, 0, 1);

/* Topic strings built at init from the device ID.
 *
 * All D2C topics are versioned with a "v1/" prefix so the cloud can route
 * different schema versions independently and devices can be migrated
 * topic-by-topic without a flag day.
 *
 * Topic map:
 *   v1/devices/{id}/telemetry    — app sensor data (temperature, humidity…)
 *   v1/devices/{id}/diagnostics  — SDK system metrics (_rssi, _reboot, _lte…)
 *   v1/devices/{id}/location     — cellular location fixes (_loc_*)
 *   v1/devices/{id}/logs         — log stream entries (_log array)
 *   v1/devices/{id}/alerts       — app-triggered threshold alerts
 *   v1/devices/{id}/commands/ack — command ACK (queued for reliable delivery)
 *   v1/devices/{id}/config/ack   — config ACK (queued for reliable delivery)
 *   v1/devices/{id}/commands     — C2D commands (subscribe)
 *   v1/devices/{id}/config       — C2D config pushes (subscribe)
 */
#define TOPIC_PREFIX "v1/"
#define TOPIC_LEN    80

static char telemetry_topic[TOPIC_LEN];    /* v1/devices/{id}/telemetry    */
static char diagnostics_topic[TOPIC_LEN];  /* v1/devices/{id}/diagnostics  */
static char location_topic[TOPIC_LEN];     /* v1/devices/{id}/location     */
static char logs_topic[TOPIC_LEN];         /* v1/devices/{id}/logs         */
static char alerts_topic[TOPIC_LEN];       /* v1/devices/{id}/alerts       */
static char command_topic[TOPIC_LEN];      /* v1/devices/{id}/commands     */
static char config_topic[TOPIC_LEN];       /* v1/devices/{id}/config       */
static char cmd_ack_topic[TOPIC_LEN];      /* v1/devices/{id}/commands/ack */
static char cfg_ack_topic[TOPIC_LEN];      /* v1/devices/{id}/config/ack   */

/* ── ACK retry queue ──────────────────────────────────────────────────────
 *
 * When the connection drops between PUBACK and the dashboard ACK publish,
 * the ACK is lost and the dashboard shows "delivered" forever. This ring
 * buffer stores pending ACKs and retries them on every poll cycle until
 * delivered or the retry limit is reached.
 *
 * Sized for CONFIG_CONEXIO_CLOUD_ACK_RETRY_MAX retries per slot.
 * Protected by a spinlock so it is safe to write from the MQTT event
 * handler context and read from the SDK background thread.
 */
#define ACK_QUEUE_DEPTH   8
#define ACK_PAYLOAD_MAX  128

struct pending_ack {
    char     payload[ACK_PAYLOAD_MAX];
    uint16_t len;
    uint8_t  retries;
    bool     is_config;   /* true → cfg_ack_topic, false → cmd_ack_topic */
    bool     used;
};

static struct pending_ack g_ack_queue[ACK_QUEUE_DEPTH];
static struct k_spinlock  g_ack_lock;

/* Broker hostname stored at init; used when (re)connecting */
static char g_broker_host[128];

/* ── ACK helpers ──────────────────────────────────────────────────────────
 *
 * After the SDK executes a command or applies a config push it calls these
 * helpers to publish an acknowledgement back to the cloud.  The dashboard
 * uses these ACKs to flip the command/config status from "delivered" to
 * "acknowledged" or "applied".
 *
 * Command ACK payload (devices/<id>/commands/ack):
 *   { "commandId": "...", "sk": "...", "result": "executed" }
 *
 * Config ACK payload (devices/<id>/config/ack):
 *   { "configId": "...", "success": true }
 */

/* ── ACK queue helpers ────────────────────────────────────────────────────
 *
 * transport_queue_ack() — add an ACK to the retry queue.
 * transport_drain_ack_queue() — attempt delivery of all pending ACKs;
 *   called from transport_poll() on every cycle.
 *
 * If delivery succeeds the slot is freed. If it fails (not connected or
 * mqtt_publish error) the slot stays and is retried next poll cycle.
 * After CONFIG_CONEXIO_CLOUD_ACK_RETRY_MAX retries the slot is dropped
 * with a warning — the telemetry fallback path on the cloud side handles
 * permanent ACK loss for FOTA; for commands the cloud timeout applies.
 */

void transport_queue_ack(bool is_config, const char *payload, size_t len)
{
    if (!payload || len == 0 || len >= ACK_PAYLOAD_MAX) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&g_ack_lock);

    /* Find a free slot */
    for (int i = 0; i < ACK_QUEUE_DEPTH; i++) {
        if (!g_ack_queue[i].used) {
            memcpy(g_ack_queue[i].payload, payload, len);
            g_ack_queue[i].payload[len] = '\0';
            g_ack_queue[i].len       = (uint16_t)len;
            g_ack_queue[i].retries   = 0;
            g_ack_queue[i].is_config = is_config;
            g_ack_queue[i].used      = true;
            k_spin_unlock(&g_ack_lock, key);
            LOG_DBG("ACK queued (%s, %zu bytes)",
                    is_config ? "config" : "command", len);
            return;
        }
    }

    k_spin_unlock(&g_ack_lock, key);
    LOG_WRN("ACK queue full — dropping ACK (increase ACK_QUEUE_DEPTH)");
}

void transport_drain_ack_queue(void)
{
    if (!connected) {
        return;
    }

    for (int i = 0; i < ACK_QUEUE_DEPTH; i++) {
        k_spinlock_key_t key = k_spin_lock(&g_ack_lock);

        if (!g_ack_queue[i].used) {
            k_spin_unlock(&g_ack_lock, key);
            continue;
        }

        /* Snapshot under lock so we can publish without holding spinlock
         * (mqtt_publish can block briefly on the MQTT TX buffer). */
        struct pending_ack snap = g_ack_queue[i];

        k_spin_unlock(&g_ack_lock, key);

        const char *topic = snap.is_config ? cfg_ack_topic : cmd_ack_topic;

        struct mqtt_publish_param msg = {
            .message.topic.qos        = MQTT_QOS_1_AT_LEAST_ONCE,
            .message.topic.topic.utf8 = (uint8_t *)topic,
            .message.topic.topic.size = strlen(topic),
            .message.payload.data     = (uint8_t *)snap.payload,
            .message.payload.len      = snap.len,
            .message_id               = (uint16_t)(k_uptime_get_32() & 0xFFFF),
            .dup_flag                 = 0,
            .retain_flag              = 0,
        };

        int ret = mqtt_publish(&client, &msg);

        key = k_spin_lock(&g_ack_lock);

        if (ret == 0) {
            /* Delivered — free slot */
            g_ack_queue[i].used = false;
            LOG_DBG("ACK delivered (%s)", snap.is_config ? "config" : "cmd");
        } else {
            g_ack_queue[i].retries++;
            if (g_ack_queue[i].retries >= CONFIG_CONEXIO_CLOUD_ACK_RETRY_MAX) {
                LOG_WRN("ACK dropped after %d retries (%s)",
                        g_ack_queue[i].retries,
                        snap.is_config ? "config" : "cmd");
                g_ack_queue[i].used = false;
            } else {
                LOG_DBG("ACK retry %d/%d (%s)",
                        g_ack_queue[i].retries,
                        CONFIG_CONEXIO_CLOUD_ACK_RETRY_MAX,
                        snap.is_config ? "config" : "cmd");
            }
        }

        k_spin_unlock(&g_ack_lock, key);
    }
}

/* Public wrapper — called from conexio_cloud.c after all settings processed.
 * Queues for reliable delivery instead of fire-and-forget. */
void transport_config_ack(const char *config_id, bool success)
{
    if (!config_id) return;

    cJSON *ack = cJSON_CreateObject();
    cJSON_AddStringToObject(ack, "configId", config_id);
    cJSON_AddBoolToObject(ack, "success", success);
    char *json = cJSON_PrintUnformatted(ack);
    cJSON_Delete(ack);
    if (!json) return;

    transport_queue_ack(true, json, strlen(json));
    cJSON_free(json);
}

/* ── MQTT event handler ───────────────────────────────────────────────────
 *
 * The Zephyr MQTT stack calls this from the application thread whenever
 * an MQTT event occurs.  We handle the four events we care about:
 *
 *   CONNACK   — connection accepted by broker.
 *               Subscribe to the commands and config topics, then notify
 *               the SDK core via transport_on_connected().
 *
 *   DISCONNECT — broker closed the connection (keepalive expired, network
 *               loss, etc.).  Notify the SDK so it can reconnect.
 *
 *   PUBLISH   — incoming message from the cloud (command or config push).
 *               Read the payload, deliver to SDK via transport_on_message(),
 *               then publish the appropriate ACK.
 *
 *   PUBACK    — acknowledgement for our outgoing QoS 1 publish.
 *               Logged at debug level; no action needed.
 */
static void mqtt_evt_handler(struct mqtt_client *c, const struct mqtt_evt *evt)
{
    int ret;

    switch (evt->type) {

    case MQTT_EVT_CONNACK:
        if (evt->result == 0) {
            LOG_INF("MQTT connected to %s", g_broker_host);
            connected = true;
            /* Unblock transport_connect() which is waiting for this */
            k_sem_give(&connack_sem);

            /* Subscribe to both topics the cloud publishes to this device:
             *   commands — Commands page and Schedules page
             *   config   — OTA Config page real-time pushes
             * Both are QoS 1 so messages are guaranteed even if the device
             * briefly drops the connection during delivery. */
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
            const struct mqtt_subscription_list sl = {
                .list       = sub_topics,
                .list_count = ARRAY_SIZE(sub_topics),
                .message_id = 1,
            };

            ret = mqtt_subscribe(c, &sl);
            if (ret) {
                LOG_ERR("Topic subscription failed (%d)", ret);
            } else {
                LOG_INF("Subscribed to: %s and %s",
                        command_topic, config_topic);
            }

            /* Notify the SDK core — triggers CONEXIO_CLOUD_EVT_CONNECTED
             * in the application and replays any offline-buffered data. */
            transport_on_connected();

        } else {
            LOG_ERR("CONNACK rejected (result %d) — check device certificate "
                    "and IoT policy attachment", evt->result);
        }
        break;

    case MQTT_EVT_DISCONNECT:
        LOG_WRN("MQTT disconnected (result %d)", evt->result);
        connected = false;
        /* Notify SDK core — triggers CONEXIO_CLOUD_EVT_DISCONNECTED and
         * exponential-backoff reconnect in the background thread. */
        transport_on_disconnected();
        break;

    case MQTT_EVT_PUBLISH: {
        /*
         * Incoming QoS 1 message from the cloud.
         *
         * Flow:
         *   1. Read payload bytes into payload_buf.
         *   2. Null-terminate so cJSON can parse it.
         *   3. Extract commandId / configId / sk (needed for ACKs).
         *   4. Send MQTT PUBACK — MUST happen before dispatch so AWS stops
         *      retrying even if the command causes a reboot.
         *   5. Send dashboard ACK (commands/ack or config/ack topic).
         *   6. 200 ms flush — ensures PUBACK bytes leave the modem.
         *   7. Deliver to SDK core (dispatches to app command/setting handlers).
         */
        const struct mqtt_publish_param *p = &evt->param.publish;
        size_t plen = MIN(p->message.payload.len, sizeof(payload_buf) - 1);

        ret = mqtt_read_publish_payload_blocking(c, payload_buf, plen);
        if (ret <= 0) {
            LOG_WRN("Failed to read MQTT payload (%d)", ret);
            break;
        }
        payload_buf[ret] = '\0';

        /* Parse enough to extract IDs for ACK before handing off.
         * config_id is extracted here but the config ACK is sent AFTER
         * dispatch (via transport_config_ack in conexio_cloud.c) so it
         * reflects the real handler result. It is not used in this scope. */
        cJSON *msg       = cJSON_Parse((char *)payload_buf);
        const char *type       = msg ? cJSON_GetStringValue(cJSON_GetObjectItem(msg, "type"))      : NULL;
        const char *command_id = msg ? cJSON_GetStringValue(cJSON_GetObjectItem(msg, "commandId")) : NULL;
        const char *sk         = msg ? cJSON_GetStringValue(cJSON_GetObjectItem(msg, "sk"))        : NULL;
        (void)(msg ? cJSON_GetObjectItem(msg, "configId") : NULL); /* extracted in SDK after dispatch */

        LOG_INF("MQTT message received on topic: %.*s (%d bytes)",
                (int)p->message.topic.topic.size,
                (char *)p->message.topic.topic.utf8,
                ret);

        /*
         * CRITICAL ORDER — PUBACK and dashboard ACK MUST be sent BEFORE
         * dispatching the command to the application.
         *
         * If we dispatch first and the command handler calls sys_reboot()
         * (e.g. REBOOT command), the device resets before the PUBACK is
         * sent. AWS IoT Core never receives the PUBACK and redelivers the
         * QoS 1 message on the next connect → infinite reboot loop.
         *
         * Correct order:
         *   1. Send MQTT PUBACK    — tells AWS "message received, stop retrying"
         *   2. Send dashboard ACK  — updates Command History status
         *   3. Small flush delay   — gives the MQTT stack time to transmit
         *                           both ACK packets before the modem is reset
         *   4. Dispatch command    — now safe to reboot / do anything
         */

        /* Step 1: Complete the QoS 1 handshake FIRST */
        if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
            struct mqtt_puback_param ack = { .message_id = p->message_id };
            int puback_ret = mqtt_publish_qos1_ack(c, &ack);
            if (puback_ret) {
                LOG_WRN("PUBACK send failed (%d) — broker may redeliver", puback_ret);
            } else {
                LOG_DBG("PUBACK sent (msg id %d)", p->message_id);
            }
        }

        /* Step 2: Send dashboard ACK for commands only.
         * Config ACK is sent AFTER dispatch (in transport_on_message) so
         * it reflects the real result from the setting handlers. */
        if (type && strcmp(type, "command") == 0) {
            cJSON *ack = cJSON_CreateObject();
            cJSON_AddStringToObject(ack, "commandId", command_id ? command_id : "");
            if (sk) cJSON_AddStringToObject(ack, "sk", sk);
            cJSON_AddStringToObject(ack, "result", "executed");
            char *ack_json = cJSON_PrintUnformatted(ack);
            cJSON_Delete(ack);
            if (ack_json) {
                /* Queue for reliable delivery — retried if connection drops */
                transport_queue_ack(false, ack_json, strlen(ack_json));
                cJSON_free(ack_json);
            }
        }
        /* Config ACK is intentionally deferred — see transport_config_ack() */

        if (msg) cJSON_Delete(msg);

        /* Step 3: Flush — give the MQTT stack time to transmit the PUBACK
         * and dashboard ACK before the command handler runs.
         * 200 ms is enough for the modem to clock out both small packets
         * over the TLS session. The builtin_on_reboot() handler already
         * waits 500 ms before calling sys_reboot(), but that delay starts
         * AFTER dispatch — this flush ensures the wire-level bytes are
         * gone before any reset occurs. */
        k_sleep(K_MSEC(200));

        /* Step 4: Dispatch — safe to reboot now */
        transport_on_message((char *)payload_buf, ret);

        break;
    }

    case MQTT_EVT_PUBACK:
        /* Our outgoing publish was acknowledged by the broker — no action */
        LOG_DBG("PUBACK received (msg id %d)", evt->param.puback.message_id);
        break;

    case MQTT_EVT_SUBACK:
        LOG_INF("SUBACK received — subscribed to commands + config topics");
        break;

    case MQTT_EVT_PINGRESP:
        /* MQTT keepalive response — connection is still alive */
        break;

    default:
        LOG_DBG("Unhandled MQTT event type: %d", evt->type);
        break;
    }
}

/* ── Transport interface implementation ───────────────────────────────────*/

/*
 * transport_init_with_config — initialise transport using runtime config.
 *
 * Called once by conexio_cloud_init() after config_fetch() and
 * cert_store_provision_from_config() have both succeeded.
 *
 * Stores the broker hostname and builds the topic strings for this device.
 * No network connection is made here — that happens in transport_connect().
 */
int transport_init_with_config(const char *device_id,
                               const struct conexio_cloud_config_t *cfg)
{
    /* Store broker host so transport_connect() can resolve it later */
    strncpy(g_broker_host, cfg->mqtt_host, sizeof(g_broker_host) - 1);

    /* Build per-device MQTT topic strings with v1/ version prefix.
     * The prefix allows the cloud to route different schema versions
     * independently — a v2 device can coexist with v1 devices without
     * any IoT Rule or Lambda changes until the migration is complete.
     *
     * D2C topics (publish):
     *   v1/devices/{id}/telemetry    — app sensor data
     *   v1/devices/{id}/diagnostics  — SDK system metrics
     *   v1/devices/{id}/location     — cellular location fixes
     *   v1/devices/{id}/logs         — log stream entries
     *   v1/devices/{id}/alerts       — app-triggered threshold alerts
     *   v1/devices/{id}/commands/ack — command ACK
     *   v1/devices/{id}/config/ack   — config push ACK
     *
     * C2D topics (subscribe):
     *   v1/devices/{id}/commands     — commands from Conexio Console
     *   v1/devices/{id}/config       — OTA Config pushes
     */
    snprintf(telemetry_topic,   sizeof(telemetry_topic),
             TOPIC_PREFIX "devices/%s/telemetry",   device_id);
    snprintf(diagnostics_topic, sizeof(diagnostics_topic),
             TOPIC_PREFIX "devices/%s/diagnostics", device_id);
    snprintf(location_topic,    sizeof(location_topic),
             TOPIC_PREFIX "devices/%s/location",    device_id);
    snprintf(logs_topic,        sizeof(logs_topic),
             TOPIC_PREFIX "devices/%s/logs",        device_id);
    snprintf(alerts_topic,      sizeof(alerts_topic),
             TOPIC_PREFIX "devices/%s/alerts",      device_id);
    snprintf(command_topic,     sizeof(command_topic),
             TOPIC_PREFIX "devices/%s/commands",    device_id);
    snprintf(config_topic,      sizeof(config_topic),
             TOPIC_PREFIX "devices/%s/config",      device_id);
    snprintf(cmd_ack_topic,     sizeof(cmd_ack_topic),
             TOPIC_PREFIX "devices/%s/commands/ack", device_id);
    snprintf(cfg_ack_topic,     sizeof(cfg_ack_topic),
             TOPIC_PREFIX "devices/%s/config/ack",  device_id);

    LOG_INF("MQTT topics (v1) for device %s:", device_id);
    LOG_DBG("  TX telemetry   : %s", telemetry_topic);
    LOG_DBG("  TX diagnostics : %s", diagnostics_topic);
    LOG_DBG("  TX location    : %s", location_topic);
    LOG_DBG("  TX logs        : %s", logs_topic);
    LOG_DBG("  TX alerts      : %s", alerts_topic);
    LOG_DBG("  RX commands    : %s", command_topic);
    LOG_DBG("  RX config      : %s", config_topic);
    LOG_DBG("  TX cmd ACK     : %s", cmd_ack_topic);
    LOG_DBG("  TX cfg ACK     : %s", cfg_ack_topic);
    return 0;
}

/*
 * transport_connect — resolve broker address and open MQTT connection.
 *
 * Called by the SDK background thread whenever transport_is_connected()
 * returns false.  The thread retries with exponential backoff on failure.
 *
 * Steps:
 *   1. DNS-resolve the broker hostname (modem DNS).
 *   2. Configure the MQTT client struct (TLS, keepalive, buffers, etc.).
 *   3. Call mqtt_connect() — this initiates the TLS handshake + CONNACK.
 *      The CONNACK event fires asynchronously in mqtt_evt_handler().
 */
int transport_connect(void)
{
    /* DNS resolution — converts hostname to IPv4 address */
    struct zsock_addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct zsock_addrinfo *res;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", BROKER_PORT);

    int ret = zsock_getaddrinfo(g_broker_host, port_str, &hints, &res);
    if (ret) {
        LOG_ERR("DNS resolution failed for %s (%d)", g_broker_host, ret);
        return -ENOENT;
    }
    memcpy(&broker_addr, res->ai_addr, res->ai_addrlen);
    zsock_freeaddrinfo(res);

    /* Retrieve device ID from the SDK core — used as the MQTT client ID.
     * AWS IoT Core requires client ID == IoT Thing name for policy enforcement. */
    extern const char *conexio_cloud_device_id(void);
    const char *dev_id = conexio_cloud_device_id();

    /* ── Configure MQTT client ──────────────────────────────────────────*/
    mqtt_client_init(&client);
    client.broker           = &broker_addr;
    client.evt_cb           = mqtt_evt_handler; /* All MQTT events handled here */
    client.client_id.utf8   = (uint8_t *)dev_id;
    client.client_id.size   = strlen(dev_id);
    client.password         = NULL;             /* Auth is via client cert */
    client.user_name        = NULL;
    client.protocol_version = MQTT_VERSION_3_1_1;
    client.keepalive        = CONFIG_CONEXIO_CLOUD_MQTT_KEEPALIVE_SEC; /* default 120 s */
    client.clean_session    = 0; /* Persistent session — broker queues QoS 1
                                  * messages sent while the device is offline.
                                  * AWS IoT Core will deliver them on reconnect.
                                  * Required for reliable command delivery. */
    client.rx_buf           = rx_buf;
    client.rx_buf_size      = sizeof(rx_buf);
    client.tx_buf           = tx_buf;
    client.tx_buf_size      = sizeof(tx_buf);
    client.transport.type   = MQTT_TRANSPORT_SECURE; /* TLS mandatory       */

    /* TLS configuration — references the modem security tags */
    struct mqtt_sec_config *tls = &client.transport.tls.config;
    tls->peer_verify    = TLS_PEER_VERIFY_REQUIRED; /* Verify broker cert   */
    tls->cipher_count   = 0;     /* 0 = use modem default cipher suite      */
    tls->cipher_list    = NULL;
    tls->sec_tag_list   = sec_tags;
    tls->sec_tag_count  = ARRAY_SIZE(sec_tags);
    tls->hostname       = g_broker_host;            /* SNI hostname         */
    tls->session_cache  = TLS_SESSION_CACHE_DISABLED; /* Fresh TLS each time */

    /* NCS v3.2.1: mqtt_disconnect() does NOT reset transport.tls.sock to -1.
     * If this is a reconnect, the stale fd causes -EADDRINUSE on next connect. */
    client.transport.tls.sock = -1;

    /* Reset the CONNACK semaphore before connecting */
    k_sem_reset(&connack_sem);

    ret = mqtt_connect(&client);
    if (ret) {
        LOG_ERR("mqtt_connect() failed (%d)", ret);
        return ret;
    }
    LOG_INF("Connecting to MQTT broker %s:%d", g_broker_host, BROKER_PORT);

    /* Poll the socket until CONNACK arrives (or timeout after 10s).
     * Without this loop the cloud thread would call transport_connect()
     * again immediately since connected==false, causing -EADDRINUSE. */
    int64_t deadline = k_uptime_get() + 10000;
    while (!connected && k_uptime_get() < deadline) {
        struct zsock_pollfd pfd = {
            .fd     = client.transport.tls.sock,
            .events = ZSOCK_POLLIN,
        };
        int rc = zsock_poll(&pfd, 1, 500);
        if (rc > 0 && (pfd.revents & ZSOCK_POLLIN)) {
            mqtt_input(&client);
        }
    }

    if (!connected) {
        LOG_ERR("CONNACK timeout — broker did not respond within 10s");
        mqtt_disconnect(&client, NULL);
        client.transport.tls.sock = -1;
        return -ETIMEDOUT;
    }

    return 0;
}

/* transport_disconnect — close the MQTT session gracefully.
 * NCS v3.2.1: mqtt_disconnect() takes a second arg (disconnect params).
 * Pass NULL for a normal disconnect with no reason code. */
int transport_disconnect(void)
{
    connected = false;
    return mqtt_disconnect(&client, NULL);
}

/* transport_is_connected — returns current connection state */
bool transport_is_connected(void)
{
    return connected;
}

/*
 * transport_publish — send a telemetry payload to the cloud.
 *
 * The payload is a null-terminated JSON string produced by build_payload()
 * in conexio_cloud.c.  It is published to the telemetry topic at QoS 1
 * so the broker acknowledges delivery.
 *
 * @param payload  JSON string to publish.
 * @param len      Length of payload in bytes.
 * @return 0 on success, -ENOTCONN if not connected, mqtt error code otherwise.
 */
int transport_publish(const char *payload, size_t len)
{
    return transport_publish_to(TOPIC_CAT_TELEMETRY, payload, len);
}

int transport_publish_to(char category, const char *payload, size_t len)
{
    if (!connected) return -ENOTCONN;

    const char *topic;

    switch (category) {
    case TOPIC_CAT_DIAGNOSTICS: topic = diagnostics_topic; break;
    case TOPIC_CAT_LOCATION:    topic = location_topic;    break;
    case TOPIC_CAT_LOGS:        topic = logs_topic;        break;
    case TOPIC_CAT_TELEMETRY:   /* fall-through */
    default:                    topic = telemetry_topic;   break;
    }

    struct mqtt_publish_param msg = {
        .message.topic.qos        = MQTT_QOS_1_AT_LEAST_ONCE,
        .message.topic.topic.utf8 = (uint8_t *)topic,
        .message.topic.topic.size = strlen(topic),
        .message.payload.data     = (uint8_t *)payload,
        .message.payload.len      = len,
        .message_id               = (uint16_t)(k_uptime_get_32() & 0xFFFF),
        .dup_flag                 = 0,
        .retain_flag              = 0,
    };

    int ret = mqtt_publish(&client, &msg);
    if (ret) {
        LOG_ERR("mqtt_publish to '%s' failed (%d)", topic, ret);
    }
    return ret;
}

int transport_publish_alert(const char *payload, size_t len)
{
    if (!connected || !payload) return -ENOTCONN;

    struct mqtt_publish_param msg = {
        .message.topic.qos        = MQTT_QOS_1_AT_LEAST_ONCE,
        .message.topic.topic.utf8 = (uint8_t *)alerts_topic,
        .message.topic.topic.size = strlen(alerts_topic),
        .message.payload.data     = (uint8_t *)payload,
        .message.payload.len      = len,
        .message_id               = (uint16_t)(k_uptime_get_32() & 0xFFFF),
        .dup_flag                 = 0,
        .retain_flag              = 0,
    };

    int ret = mqtt_publish(&client, &msg);
    if (ret) {
        LOG_ERR("Alert publish to '%s' failed (%d)", alerts_topic, ret);
    } else {
        LOG_DBG("Alert published (%zu bytes)", len);
    }
    return ret;
}

/*
 * transport_publish_raw — publish payload to an arbitrary MQTT topic (QoS 1).
 *
 * Used by fota.c to publish AWS IoT Jobs status updates to
 * $aws/things/{deviceId}/jobs/{jobId}/update without coupling fota.c
 * to the MQTT client directly.
 *
 * @param topic    Full MQTT topic string (null-terminated).
 * @param payload  Payload bytes to publish.
 * @param len      Length of payload in bytes.
 * @return 0 on success, -ENOTCONN if not connected, MQTT error otherwise.
 */
int transport_publish_raw(const char *topic, const char *payload, size_t len)
{
    if (!connected || !topic || !payload) return -ENOTCONN;

    struct mqtt_publish_param msg = {
        .message.topic.qos        = MQTT_QOS_1_AT_LEAST_ONCE,
        .message.topic.topic.utf8 = (uint8_t *)topic,
        .message.topic.topic.size = strlen(topic),
        .message.payload.data     = (uint8_t *)payload,
        .message.payload.len      = len,
        .message_id               = (uint16_t)(k_uptime_get_32() & 0xFFFF),
        .dup_flag                 = 0,
        .retain_flag              = 0,
    };

    int ret = mqtt_publish(&client, &msg);
    if (ret) {
        LOG_ERR("transport_publish_raw: mqtt_publish to '%s' failed (%d)", topic, ret);
    }
    return ret;
}

/*
 * transport_poll — drive the MQTT event loop for one timeout period.
 *
 * Called repeatedly by the SDK background thread.  It:
 *   1. Waits up to `timeout` for incoming data on the MQTT socket.
 *   2. If data arrives, calls mqtt_input() to parse it and fire events.
 *   3. Calls mqtt_live() to send keepalive PINGREQs when due.
 *
 * If either call returns an error (other than -EAGAIN) the connection is
 * considered lost and transport_on_disconnected() is called.
 */
void transport_poll(k_timeout_t timeout)
{
    if (!connected) return;

    /* Wait for incoming data on the MQTT socket */
    struct zsock_pollfd fds = {
        .fd     = client.transport.tls.sock,
        .events = ZSOCK_POLLIN,
    };

    int ret = zsock_poll(&fds, 1, k_ticks_to_ms_floor32(timeout.ticks));

    if (ret > 0 && (fds.revents & ZSOCK_POLLIN)) {
        /* Data available — parse MQTT packet(s) and fire event handler */
        ret = mqtt_input(&client);
        if (ret) {
            LOG_WRN("mqtt_input error (%d) — connection lost", ret);
            connected = false;
            transport_on_disconnected();
            return;
        }
    }

    /* Send PINGREQ if the keepalive timer is due */
    ret = mqtt_live(&client);
    if (ret && ret != -EAGAIN) {
        LOG_WRN("mqtt_live error (%d) — connection lost", ret);
        connected = false;
        transport_on_disconnected();
        return;
    }

    /* Drain any pending ACKs — retries command/config ACKs that failed
     * on a previous poll cycle due to a transient connection drop. */
    transport_drain_ack_queue();
}
