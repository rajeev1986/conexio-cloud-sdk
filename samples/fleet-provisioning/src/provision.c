/*
 * provision.c — AWS IoT Fleet Provisioning state machine
 *
 * nRF Connect SDK v3.2.1 / nRF91xx
 *
 * AWS Fleet Provisioning MQTT API topics:
 *
 *   Step 1 — CreateKeysAndCertificate
 *     Publish to:   $aws/certificates/create/json
 *     Subscribe to: $aws/certificates/create/json/accepted
 *                   $aws/certificates/create/json/rejected
 *     Response contains: certificatePem, privateKey, certificateOwnershipToken
 *
 *   Step 2 — RegisterThing
 *     Publish to:   $aws/provisioning-templates/<template>/provision/json
 *     Subscribe to: $aws/provisioning-templates/<template>/provision/json/accepted
 *                   $aws/provisioning-templates/<template>/provision/json/rejected
 *     Request contains: certificateOwnershipToken + templateParameters (deviceId, etc.)
 *     Response contains: thingName, deviceConfiguration
 */

#include <zephyr/kernel.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>
#include <cJSON.h>
#include <cJSON_os.h>
#include <string.h>
#include <stdio.h>

#include "provision.h"
#include "cert_store.h"

LOG_MODULE_REGISTER(provision, LOG_LEVEL_INF);

/* ── Topic strings ───────────────────────────────────────────────────────── */
#define TOPIC_CREATE_KEYS       "$aws/certificates/create/json"
#define TOPIC_CREATE_ACCEPTED   "$aws/certificates/create/json/accepted"
#define TOPIC_CREATE_REJECTED   "$aws/certificates/create/json/rejected"

/* RegisterThing topics are built at runtime using the template name */
static char topic_register[128];
static char topic_register_accepted[140];
static char topic_register_rejected[140];

/* ── Provisioning state machine ──────────────────────────────────────────── */
typedef enum {
    PROV_STATE_IDLE,
    PROV_STATE_WAITING_CREATE,      /* waiting for CreateKeysAndCertificate response */
    PROV_STATE_WAITING_REGISTER,    /* waiting for RegisterThing response */
    PROV_STATE_DONE,
    PROV_STATE_FAILED,
} prov_state_t;

static prov_state_t prov_state = PROV_STATE_IDLE;

/* ── Provisioning result buffers ─────────────────────────────────────────── */
/* Certificates can be ~1.5 KB each. Use static buffers to avoid heap churn. */
#define CERT_BUF_SIZE  2048
#define KEY_BUF_SIZE   2048
#define TOKEN_BUF_SIZE 512

static char s_certificate_pem[CERT_BUF_SIZE];
static char s_private_key[KEY_BUF_SIZE];
static char s_ownership_token[TOKEN_BUF_SIZE];
static char s_thing_name[64];

/* ── MQTT payload receive buffer ─────────────────────────────────────────── */
#define PAYLOAD_BUF_SIZE 4096
static uint8_t payload_buf[PAYLOAD_BUF_SIZE];

/* ── Semaphore to signal provisioning completion to main loop ────────────── */
static K_SEM_DEFINE(prov_done_sem, 0, 1);

/* ─────────────────────────────────────────────────────────────────────────── */
/* Internal helpers                                                            */
/* ─────────────────────────────────────────────────────────────────────────── */

static int mqtt_publish_empty(struct mqtt_client *client, const char *topic)
{
    struct mqtt_publish_param msg = {
        .message.topic.qos        = MQTT_QOS_1_AT_LEAST_ONCE,
        .message.topic.topic.utf8 = (uint8_t *)topic,
        .message.topic.topic.size = strlen(topic),
        .message.payload.data     = (uint8_t *)"{}",
        .message.payload.len      = 2,
        .message_id               = (uint16_t)(k_uptime_get_32() & 0xFFFF),
        .dup_flag                 = 0,
        .retain_flag              = 0,
    };
    return mqtt_publish(client, &msg);
}

static int mqtt_publish_str(struct mqtt_client *client,
                            const char *topic,
                            const char *payload)
{
    struct mqtt_publish_param msg = {
        .message.topic.qos        = MQTT_QOS_1_AT_LEAST_ONCE,
        .message.topic.topic.utf8 = (uint8_t *)topic,
        .message.topic.topic.size = strlen(topic),
        .message.payload.data     = (uint8_t *)payload,
        .message.payload.len      = strlen(payload),
        .message_id               = (uint16_t)(k_uptime_get_32() & 0xFFFF),
        .dup_flag                 = 0,
        .retain_flag              = 0,
    };
    return mqtt_publish(client, &msg);
}

static int subscribe_provisioning_topics(struct mqtt_client *client)
{
    /* Subscribe to all four response topics at once */
    struct mqtt_topic topics[] = {
        {
            .topic = { .utf8 = (uint8_t *)TOPIC_CREATE_ACCEPTED,
                       .size = strlen(TOPIC_CREATE_ACCEPTED) },
            .qos = MQTT_QOS_1_AT_LEAST_ONCE,
        },
        {
            .topic = { .utf8 = (uint8_t *)TOPIC_CREATE_REJECTED,
                       .size = strlen(TOPIC_CREATE_REJECTED) },
            .qos = MQTT_QOS_1_AT_LEAST_ONCE,
        },
        {
            .topic = { .utf8 = (uint8_t *)topic_register_accepted,
                       .size = strlen(topic_register_accepted) },
            .qos = MQTT_QOS_1_AT_LEAST_ONCE,
        },
        {
            .topic = { .utf8 = (uint8_t *)topic_register_rejected,
                       .size = strlen(topic_register_rejected) },
            .qos = MQTT_QOS_1_AT_LEAST_ONCE,
        },
    };

    const struct mqtt_subscription_list sub_list = {
        .list       = topics,
        .list_count = ARRAY_SIZE(topics),
        .message_id = 100,
    };

    int ret = mqtt_subscribe(client, &sub_list);
    if (ret) {
        LOG_ERR("subscribe_provisioning_topics failed (err %d)", ret);
    } else {
        LOG_INF("Subscribed to all provisioning response topics");
    }
    return ret;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Handle CreateKeysAndCertificate/accepted                                   */
/*                                                                             */
/* Response JSON:                                                              */
/* {                                                                           */
/*   "certificateId": "abc...",                                               */
/*   "certificatePem": "-----BEGIN CERTIFICATE-----\n...",                    */
/*   "privateKey": "-----BEGIN RSA PRIVATE KEY-----\n...",                    */
/*   "certificateOwnershipToken": "eyJ..."                                    */
/* }                                                                           */
/* ─────────────────────────────────────────────────────────────────────────── */

static void handle_create_accepted(struct mqtt_client *client,
                                   const char *payload,
                                   const char *device_id)
{
    LOG_INF("CreateKeysAndCertificate accepted — parsing response...");

    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        LOG_ERR("Failed to parse CreateKeysAndCertificate response");
        prov_state = PROV_STATE_FAILED;
        k_sem_give(&prov_done_sem);
        return;
    }

    const char *cert  = cJSON_GetStringValue(cJSON_GetObjectItem(root, "certificatePem"));
    const char *key   = cJSON_GetStringValue(cJSON_GetObjectItem(root, "privateKey"));
    const char *token = cJSON_GetStringValue(cJSON_GetObjectItem(root, "certificateOwnershipToken"));

    if (!cert || !key || !token) {
        LOG_ERR("CreateKeysAndCertificate response missing required fields");
        cJSON_Delete(root);
        prov_state = PROV_STATE_FAILED;
        k_sem_give(&prov_done_sem);
        return;
    }

    /* Stash in static buffers for use in RegisterThing and cert_store */
    strncpy(s_certificate_pem, cert,  sizeof(s_certificate_pem) - 1);
    strncpy(s_private_key,     key,   sizeof(s_private_key) - 1);
    strncpy(s_ownership_token, token, sizeof(s_ownership_token) - 1);

    LOG_INF("Received unique certificate and private key from AWS");
    cJSON_Delete(root);

    /* ── Step 2: RegisterThing ───────────────────────────────────────────── */
    /* Send the ownership token + our device parameters so AWS can:
     *   - Activate the certificate
     *   - Create an IoT Thing with our device ID as the name
     *   - Attach the policy defined in the provisioning template
     */
    cJSON *req    = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();

    cJSON_AddStringToObject(req, "certificateOwnershipToken", s_ownership_token);
    cJSON_AddItemToObject(req, "parameters", params);

    /* templateParameters — must match what your provisioning template expects.
     * The CDK template uses SerialNumber as the Thing name. */
    cJSON_AddStringToObject(params, "SerialNumber", device_id);

    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    if (!req_str) {
        LOG_ERR("Failed to serialise RegisterThing request");
        prov_state = PROV_STATE_FAILED;
        k_sem_give(&prov_done_sem);
        return;
    }

    LOG_INF("Publishing RegisterThing request for device: %s", device_id);
    int ret = mqtt_publish_str(client, topic_register, req_str);
    cJSON_FreeString(req_str);

    if (ret) {
        LOG_ERR("RegisterThing publish failed (err %d)", ret);
        prov_state = PROV_STATE_FAILED;
        k_sem_give(&prov_done_sem);
        return;
    }

    prov_state = PROV_STATE_WAITING_REGISTER;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Handle RegisterThing/accepted                                               */
/*                                                                             */
/* Response JSON:                                                              */
/* {                                                                           */
/*   "thingName": "123456789012345",                                 */
/*   "deviceConfiguration": {}                                                */
/* }                                                                           */
/* ─────────────────────────────────────────────────────────────────────────── */

static void handle_register_accepted(const char *payload)
{
    LOG_INF("RegisterThing accepted — provisioning complete");

    cJSON *root = cJSON_Parse(payload);
    if (root) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "thingName"));
        if (name) {
            strncpy(s_thing_name, name, sizeof(s_thing_name) - 1);
            LOG_INF("AWS Thing name: %s", s_thing_name);
        }
        cJSON_Delete(root);
    }

    /* Write the unique device credentials into modem secure storage.
     * After this call the device cert (tag 21) exists in the modem. */
    int ret = cert_store_write_device_creds(s_certificate_pem, s_private_key);
    if (ret) {
        LOG_ERR("Failed to store device credentials (err %d)", ret);
        prov_state = PROV_STATE_FAILED;
    } else {
        LOG_INF("Device credentials written to modem — provisioning successful");
        prov_state = PROV_STATE_DONE;
    }

    k_sem_give(&prov_done_sem);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* MQTT event handler — provisioning phase                                    */
/* ─────────────────────────────────────────────────────────────────────────── */

/* Forward declaration — device_id needed in handle_create_accepted */
static const char *g_device_id;

void provision_mqtt_event_handler(struct mqtt_client *client,
                                  const struct mqtt_evt *evt)
{
    int ret;

    switch (evt->type) {

    case MQTT_EVT_CONNACK:
        if (evt->result == 0) {
            LOG_INF("MQTT connected (provisioning session)");

            /* Subscribe to provisioning response topics */
            ret = subscribe_provisioning_topics(client);
            if (ret) {
                prov_state = PROV_STATE_FAILED;
                k_sem_give(&prov_done_sem);
                break;
            }

            /* Publish CreateKeysAndCertificate — empty JSON body is correct */
            LOG_INF("Publishing CreateKeysAndCertificate request...");
            ret = mqtt_publish_empty(client, TOPIC_CREATE_KEYS);
            if (ret) {
                LOG_ERR("CreateKeysAndCertificate publish failed (err %d)", ret);
                prov_state = PROV_STATE_FAILED;
                k_sem_give(&prov_done_sem);
                break;
            }

            prov_state = PROV_STATE_WAITING_CREATE;
        } else {
            LOG_ERR("MQTT CONNACK error %d (check claim cert is attached to a policy)", evt->result);
            prov_state = PROV_STATE_FAILED;
            k_sem_give(&prov_done_sem);
        }
        break;

    case MQTT_EVT_DISCONNECT:
        LOG_WRN("MQTT disconnected during provisioning (result %d)", evt->result);
        if (prov_state != PROV_STATE_DONE) {
            prov_state = PROV_STATE_FAILED;
            k_sem_give(&prov_done_sem);
        }
        break;

    case MQTT_EVT_PUBLISH: {
        const struct mqtt_publish_param *p = &evt->param.publish;
        size_t payload_len = p->message.payload.len;

        /* Extract topic string for routing */
        char topic[256] = {0};
        size_t topic_len = MIN(p->message.topic.topic.size, sizeof(topic) - 1);
        memcpy(topic, p->message.topic.topic.utf8, topic_len);

        /* Read payload */
        if (payload_len >= sizeof(payload_buf)) {
            LOG_WRN("Payload too large (%zu bytes) — truncating", payload_len);
            payload_len = sizeof(payload_buf) - 1;
        }
        ret = mqtt_read_publish_payload_blocking(client, payload_buf, payload_len);
        if (ret < 0) {
            LOG_ERR("mqtt_read_publish_payload failed (err %d)", ret);
            break;
        }
        payload_buf[ret] = '\0';

        /* Send PUBACK */
        if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
            struct mqtt_puback_param puback = { .message_id = p->message_id };
            mqtt_publish_qos1_ack(client, &puback);
        }

        /* Route to correct handler */
        if (strcmp(topic, TOPIC_CREATE_ACCEPTED) == 0) {
            handle_create_accepted(client, (char *)payload_buf, g_device_id);

        } else if (strcmp(topic, TOPIC_CREATE_REJECTED) == 0) {
            LOG_ERR("CreateKeysAndCertificate REJECTED: %s", payload_buf);
            LOG_ERR("Check that the claim certificate has an IoT policy with "
                    "iot:CreateKeysAndCertificate permission");
            prov_state = PROV_STATE_FAILED;
            k_sem_give(&prov_done_sem);

        } else if (strcmp(topic, topic_register_accepted) == 0) {
            handle_register_accepted((char *)payload_buf);

        } else if (strcmp(topic, topic_register_rejected) == 0) {
            LOG_ERR("RegisterThing REJECTED: %s", payload_buf);
            LOG_ERR("Check the Fleet Provisioning template and pre-provisioning hook Lambda");
            prov_state = PROV_STATE_FAILED;
            k_sem_give(&prov_done_sem);

        } else {
            LOG_DBG("Unhandled provisioning topic: %s", topic);
        }
        break;
    }

    case MQTT_EVT_PUBACK:
        LOG_DBG("PUBACK (id %d)", evt->param.puback.message_id);
        break;

    case MQTT_EVT_SUBACK:
        LOG_DBG("SUBACK — provisioning topics subscribed");
        break;

    case MQTT_EVT_PINGRESP:
        break;

    default:
        LOG_DBG("Unhandled MQTT event: %d", evt->type);
        break;
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Public entry point                                                          */
/* ─────────────────────────────────────────────────────────────────────────── */

int run_provisioning(const char *device_id, struct mqtt_client *client)
{
    /* Build RegisterThing topic strings now that template name is known */
    snprintf(topic_register,
             sizeof(topic_register),
             "$aws/provisioning-templates/%s/provision/json",
             CONFIG_CONEXIO_FLEET_TEMPLATE_NAME);
    snprintf(topic_register_accepted,
             sizeof(topic_register_accepted),
             "$aws/provisioning-templates/%s/provision/json/accepted",
             CONFIG_CONEXIO_FLEET_TEMPLATE_NAME);
    snprintf(topic_register_rejected,
             sizeof(topic_register_rejected),
             "$aws/provisioning-templates/%s/provision/json/rejected",
             CONFIG_CONEXIO_FLEET_TEMPLATE_NAME);

    g_device_id = device_id;
    prov_state  = PROV_STATE_IDLE;

    LOG_INF("Starting Fleet Provisioning for device: %s", device_id);
    LOG_INF("Template: %s", CONFIG_CONEXIO_FLEET_TEMPLATE_NAME);

    /* Connect with claim credentials */
    int ret = mqtt_connect(client);
    if (ret) {
        LOG_ERR("mqtt_connect (provisioning) failed (err %d)", ret);
        return ret;
    }

    /* Drive MQTT until done or timeout (90 seconds) */
    int64_t deadline_ms = k_uptime_get() + 90000;

    while (prov_state != PROV_STATE_DONE && prov_state != PROV_STATE_FAILED) {
        if (k_uptime_get() > deadline_ms) {
            LOG_ERR("Fleet Provisioning timed out after 90 seconds");
            mqtt_disconnect(client);
            return -ETIMEDOUT;
        }

        struct zsock_pollfd fds = {
            .fd     = client->transport.tls.sock,
            .events = ZSOCK_POLLIN,
        };
        ret = zsock_poll(&fds, 1, K_SECONDS(1).ticks);
        if (ret > 0 && (fds.revents & ZSOCK_POLLIN)) {
            ret = mqtt_input(client);
            if (ret) {
                LOG_WRN("mqtt_input error (%d)", ret);
            }
        }

        ret = mqtt_live(client);
        if (ret && ret != -EAGAIN) {
            LOG_WRN("mqtt_live error (%d)", ret);
        }
    }

    mqtt_disconnect(client);

    if (prov_state == PROV_STATE_DONE) {
        LOG_INF("Fleet Provisioning completed successfully");
        return 0;
    }

    return -EACCES;
}
