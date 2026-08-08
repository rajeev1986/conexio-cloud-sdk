/*
 * provision.c — AWS IoT Fleet Provisioning via CreateCertificateFromCsr
 * NCS v3.2.1 / nRF9151
 *
 * Root cause of previous approach failure
 * ────────────────────────────────────────
 * The nRF9160/nRF9151 modem has a 2 KB TLS receive buffer limit per AT socket.
 * The CreateKeysAndCertificate response is ~3.6 KB (cert + private key + token)
 * which exceeds this limit. The modem drops the TLS record and signals POLLERR,
 * making the data permanently unavailable to the application.
 *
 * Solution: CreateCertificateFromCsr
 * ───────────────────────────────────
 * 1. Generate a key pair in the modem using AT%KEYGEN — private key never
 *    leaves the modem's secure storage.
 * 2. AT%KEYGEN returns a PKCS#10 CSR (~1 KB) which we send to AWS.
 * 3. AWS returns only the certificate (~1.2 KB) + ownership token (~0.5 KB)
 *    = ~1.7 KB total — within the 2 KB modem TLS receive buffer limit.
 * 4. RegisterThing response is ~0.1 KB — also well within limit.
 *
 * This approach is also more secure: the private key is generated and stored
 * entirely within the modem's trusted execution environment.
 *
 * Flow
 * ────
 *  1. AT%KEYGEN=21,2,0  → modem generates RSA-2048 key pair, returns CSR PEM
 *  2. MQTT connect (claim creds)
 *  3. Subscribe to create-from-csr topics
 *  4. Publish CSR to $aws/certificates/create-from-csr/json
 *  5. Receive certificate + ownership token (~1.7 KB — fits in modem buffer)
 *  6. Write certificate to modem (tag 21) via modem_key_mgmt_write
 *  7. MQTT connect again
 *  8. Subscribe to register topics
 *  9. Publish RegisterThing with ownership token + SerialNumber
 * 10. Receive accepted response → provisioning complete
 */

#include <zephyr/kernel.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/base64.h>
#include <modem/modem_key_mgmt.h>
#include <nrf_modem_at.h>
#include <cJSON.h>
#include <cJSON_os.h>
#include <string.h>
#include <stdio.h>

#include "provision.h"
#include "cert_store.h"
#include <modem/lte_lc.h>

LOG_MODULE_REGISTER(provision, LOG_LEVEL_INF);

/* ── AWS Fleet Provisioning topics ───────────────────────────────────────── */
#define TOPIC_CREATE_CSR      "$aws/certificates/create-from-csr/json"
#define TOPIC_CREATE_CSR_OK   "$aws/certificates/create-from-csr/json/accepted"
#define TOPIC_CREATE_CSR_FAIL "$aws/certificates/create-from-csr/json/rejected"

static char topic_register[128];
static char topic_register_ok[140];
static char topic_register_fail[140];

/* ── State machine ───────────────────────────────────────────────────────── */
typedef enum {
    PROV_IDLE,
    PROV_WAIT_CONNACK,
    PROV_WAIT_SUBACK,
    PROV_CONNECTED,
    PROV_GOT_CERT,
    PROV_DONE,
    PROV_FAILED,
} prov_state_t;

static volatile prov_state_t s_state;

/* ── Buffers ─────────────────────────────────────────────────────────────── */
/* CSR from AT%KEYGEN is ~1 KB */
#define CSR_BUF_SIZE   1536
/* Certificate from AWS is ~1.2 KB */
#define CERT_BUF_SIZE  2048
/* Ownership token ~512 bytes */
#define TOKEN_BUF_SIZE 512
/* MQTT publish payload buffer */
#define PUB_BUF_SIZE   2048

static char s_csr[CSR_BUF_SIZE] __aligned(4);
static char s_cert[CERT_BUF_SIZE] __aligned(4);
static char s_token[TOKEN_BUF_SIZE] __aligned(4);
static char s_cert_id[128];

/* Payload buffer — CreateCertificateFromCsr response fits within 2 KB */
static uint8_t s_payload_buf[2048] __aligned(4);

static const char *g_device_id;

/* ── AT%KEYGEN: generate key pair and get CSR ────────────────────────────── */

/*
 * generate_csr
 * ────────────
 * Issues AT%KEYGEN=<tag>,2,0 to the modem.
 * The modem generates an RSA-2048 key pair:
 *   - Private key: stored internally at the given security tag (never exported)
 *   - CSR: returned as a PEM string in the AT response
 *
 * AT%KEYGEN response format:
 *   %KEYGEN: "-----BEGIN CERTIFICATE REQUEST-----\n...\n-----END CERTIFICATE REQUEST-----\n"
 *   OK
 *
 * @param sec_tag  Security tag where the private key will be stored (e.g. 21)
 * @param csr_buf  Buffer to receive the CSR PEM string
 * @param csr_size Size of csr_buf
 * @return 0 on success, negative errno on failure
 */
static int generate_csr(int sec_tag, char *csr_buf, size_t csr_size)
{
    static char at_resp[1536] __aligned(4);
    int ret;

    LOG_INF("Generating key pair at sec_tag %d (AT%%KEYGEN)...", sec_tag);

    /* Delete any existing credential at this tag first */
    modem_key_mgmt_delete(sec_tag, MODEM_KEY_MGMT_CRED_TYPE_PUBLIC_CERT);
    modem_key_mgmt_delete(sec_tag, MODEM_KEY_MGMT_CRED_TYPE_PRIVATE_CERT);

    /*
     * AT%KEYGEN=<sec_tag>,<type>,<format>[,<subject_name>][,<san>]
     *   type   2 = RSA (nRF9160) / EC (nRF9151 — uses P-256 regardless)
     *   format 0 = CSR
     *
     * nRF9151 response format (different from nRF9160):
     *   %KEYGEN: "Base64Url(CSR_DER).Base64Url(attestation_token)"
     *
     * The CSR DER bytes are Base64URL-encoded (RFC 4648 §5: uses - and _
     * instead of + and /) and are the portion BEFORE the first dot.
     * We decode them and wrap in standard PEM headers.
     */
    ret = nrf_modem_at_cmd(at_resp, sizeof(at_resp),
                           "AT%%KEYGEN=%d,2,0", sec_tag);
    if (ret) {
        LOG_ERR("AT%%KEYGEN failed: %d (type=%d err=%d)",
                ret, nrf_modem_at_err_type(ret), nrf_modem_at_err(ret));
        return (ret > 0) ? -EIO : ret;
    }

    LOG_DBG("AT%%KEYGEN response: %.120s", at_resp);

    /*
     * Extract the Base64URL-encoded CSR (before the dot).
     * Response is one of:
     *   %KEYGEN: "Base64Url(CSR).Base64Url(token)"\r\nOK\r\n  (nRF9151)
     *   %KEYGEN: "-----BEGIN CERTIFICATE REQUEST-----\n...\n-----END..."  (nRF9160)
     */

    /* Check for PEM format first (nRF9160) */
    const char *pem_start = strstr(at_resp, "-----BEGIN CERTIFICATE REQUEST-----");
    if (pem_start) {
        const char *pem_end = strstr(pem_start,
                                     "-----END CERTIFICATE REQUEST-----");
        if (!pem_end) {
            LOG_ERR("AT%%KEYGEN: no END marker");
            return -EPROTO;
        }
        pem_end += strlen("-----END CERTIFICATE REQUEST-----");
        if (*pem_end == '\n') pem_end++;

        size_t pem_len = (size_t)(pem_end - pem_start);
        if (pem_len >= csr_size) {
            LOG_ERR("CSR PEM too large: %zu", pem_len);
            return -ENOMEM;
        }
        memcpy(csr_buf, pem_start, pem_len);
        csr_buf[pem_len] = '\0';
        LOG_INF("CSR (PEM format) generated: %zu bytes", pem_len);
        return 0;
    }

    /*
     * nRF9151 format: %KEYGEN: "Base64Url(DER).Base64Url(token)"
     * Find the opening quote, extract up to the dot.
     */
    const char *b64_start = strchr(at_resp, '"');
    if (!b64_start) {
        LOG_ERR("AT%%KEYGEN: no quote in response: %.80s", at_resp);
        return -EPROTO;
    }
    b64_start++; /* skip the quote */

    /* Find the dot separator between CSR and attestation token */
    const char *dot = strchr(b64_start, '.');
    if (!dot) {
        LOG_ERR("AT%%KEYGEN: no dot separator in response: %.80s", b64_start);
        return -EPROTO;
    }

    size_t b64_len = (size_t)(dot - b64_start);
    LOG_DBG("Base64URL CSR: %zu chars", b64_len);

    /*
     * Decode Base64URL to DER.
     * Base64URL uses '-' and '_' instead of '+' and '/'.
     * We need to add padding ('=') to make the length a multiple of 4.
     *
     * DER size = floor(b64_len * 3 / 4)
     */
    static char b64_padded[1024] __aligned(4);
    static uint8_t der_buf[768] __aligned(4);

    if (b64_len >= sizeof(b64_padded) - 4) {
        LOG_ERR("Base64URL CSR too long: %zu", b64_len);
        return -ENOMEM;
    }

    /* Copy and convert Base64URL → standard Base64 */
    memcpy(b64_padded, b64_start, b64_len);
    for (size_t i = 0; i < b64_len; i++) {
        if (b64_padded[i] == '-') b64_padded[i] = '+';
        if (b64_padded[i] == '_') b64_padded[i] = '/';
    }
    /* Add padding */
    size_t pad = (4 - (b64_len % 4)) % 4;
    for (size_t i = 0; i < pad; i++) {
        b64_padded[b64_len + i] = '=';
    }
    b64_padded[b64_len + pad] = '\0';

    /* Decode Base64 → DER using the Zephyr base64 library */
    size_t der_len = 0;
    ret = base64_decode(der_buf, sizeof(der_buf), &der_len,
                        b64_padded, b64_len + pad);
    if (ret) {
        LOG_ERR("base64_decode failed: %d", ret);
        return -EINVAL;
    }
    LOG_DBG("DER CSR: %zu bytes", der_len);

    /* Re-encode DER as standard Base64 for PEM wrapping */
    static char pem_b64[1024] __aligned(4);
    size_t pem_b64_len = 0;
    ret = base64_encode(pem_b64, sizeof(pem_b64), &pem_b64_len,
                        der_buf, der_len);
    if (ret) {
        LOG_ERR("base64_encode failed: %d", ret);
        return -EINVAL;
    }

    /* Wrap in PEM headers with 64-char line breaks */
    int written = snprintf(csr_buf, csr_size,
                           "-----BEGIN CERTIFICATE REQUEST-----\n");
    if (written < 0 || (size_t)written >= csr_size) return -ENOMEM;
    size_t pos = (size_t)written;

    for (size_t i = 0; i < pem_b64_len; i += 64) {
        size_t chunk = MIN(64u, pem_b64_len - i);
        if (pos + chunk + 2 >= csr_size) return -ENOMEM;
        memcpy(csr_buf + pos, pem_b64 + i, chunk);
        pos += chunk;
        csr_buf[pos++] = '\n';
    }

    const char *footer = "-----END CERTIFICATE REQUEST-----\n";
    size_t footer_len = strlen(footer);
    if (pos + footer_len >= csr_size) return -ENOMEM;
    memcpy(csr_buf + pos, footer, footer_len);
    pos += footer_len;
    csr_buf[pos] = '\0';

    LOG_INF("CSR (nRF9151 DER→PEM) generated: %zu bytes", pos);
    return 0;
}

/* ── Publish helpers (QoS 0) ─────────────────────────────────────────────── */

static int pub_str(struct mqtt_client *c, const char *topic, const char *payload)
{
    struct mqtt_publish_param p = {
        .message.topic.qos        = MQTT_QOS_0_AT_MOST_ONCE,
        .message.topic.topic.utf8 = (uint8_t *)topic,
        .message.topic.topic.size = strlen(topic),
        .message.payload.data     = (uint8_t *)payload,
        .message.payload.len      = strlen(payload),
        .message_id               = 0,
    };
    return mqtt_publish(c, &p);
}

/* ── Subscribe helpers ───────────────────────────────────────────────────── */

static int subscribe_create_csr(struct mqtt_client *c)
{
    struct mqtt_topic topics[] = {
        { .topic = { .utf8 = (uint8_t *)TOPIC_CREATE_CSR_OK,
                     .size = strlen(TOPIC_CREATE_CSR_OK) },
          .qos = MQTT_QOS_1_AT_LEAST_ONCE },
        { .topic = { .utf8 = (uint8_t *)TOPIC_CREATE_CSR_FAIL,
                     .size = strlen(TOPIC_CREATE_CSR_FAIL) },
          .qos = MQTT_QOS_1_AT_LEAST_ONCE },
    };
    const struct mqtt_subscription_list sl = {
        .list = topics, .list_count = ARRAY_SIZE(topics), .message_id = 100,
    };
    int ret = mqtt_subscribe(c, &sl);
    if (ret) { LOG_ERR("subscribe_create_csr failed: %d", ret); }
    else      { LOG_INF("Subscribed to CreateCertificateFromCsr topics"); }
    return ret;
}

static int subscribe_register(struct mqtt_client *c)
{
    struct mqtt_topic topics[] = {
        { .topic = { .utf8 = (uint8_t *)topic_register_ok,
                     .size = strlen(topic_register_ok) },
          .qos = MQTT_QOS_1_AT_LEAST_ONCE },
        { .topic = { .utf8 = (uint8_t *)topic_register_fail,
                     .size = strlen(topic_register_fail) },
          .qos = MQTT_QOS_1_AT_LEAST_ONCE },
    };
    const struct mqtt_subscription_list sl = {
        .list = topics, .list_count = ARRAY_SIZE(topics), .message_id = 200,
    };
    int ret = mqtt_subscribe(c, &sl);
    if (ret) { LOG_ERR("subscribe_register failed: %d", ret); }
    else      { LOG_INF("Subscribed to RegisterThing topics"); }
    return ret;
}

/* ── drive_until: poll loop until target state ───────────────────────────── */

static int drive_until(struct mqtt_client *client,
                        prov_state_t target, int32_t timeout_ms)
{
    /*
     * Snapshot the socket fd here, AFTER mqtt_connect() has returned.
     * mqtt_connect() opens the TLS socket synchronously and stores the fd
     * in client->transport.tls.sock before returning, so the value is
     * stable for the lifetime of this call.
     *
     * Do NOT capture this at the call site before mqtt_connect() — the fd
     * is only valid once mqtt_connect() has run (it is -1 beforehand).
     */
    struct zsock_pollfd pfd = {
        .fd     = client->transport.tls.sock,
        .events = ZSOCK_POLLIN,
    };
    int64_t deadline_ms = k_uptime_get() + timeout_ms;

    while (s_state != target && s_state != PROV_FAILED && s_state != PROV_DONE) {
        int64_t remaining = deadline_ms - k_uptime_get();
        if (remaining <= 0) {
            LOG_ERR("drive_until: timeout waiting for state %d", target);
            return -ETIMEDOUT;
        }

        int32_t ka = mqtt_keepalive_time_left(client);
        int32_t poll_ms = (int32_t)MIN(remaining, (int64_t)(ka > 0 ? ka : 1000));

        int rc = zsock_poll(&pfd, 1, poll_ms);
        if (rc < 0) { return -errno; }

        (void)mqtt_live(client);

        if (pfd.revents & ZSOCK_POLLIN) {
            rc = mqtt_input(client);
            if (rc && rc != -ENOTCONN) {
                LOG_WRN("drive_until: mqtt_input %d", rc);
            }
        }

        if (pfd.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL)) {
            LOG_DBG("drive_until: revents=0x%x", pfd.revents);
            mqtt_input(client);
            if (s_state == target) break;
            if (s_state != PROV_FAILED && s_state != PROV_DONE) {
                return -ECONNRESET;
            }
            break;
        }
    }
    return (s_state == PROV_FAILED) ? -EACCES : 0;
}

/* ── MQTT event handler ──────────────────────────────────────────────────── */

void provision_mqtt_evt_handler(struct mqtt_client *client,
                                const struct mqtt_evt *evt)
{
    int rc;

    switch (evt->type) {
    case MQTT_EVT_CONNACK:
        if (evt->result != 0) {
            LOG_ERR("CONNACK error %d", evt->result);
            s_state = PROV_FAILED;
        } else {
            LOG_INF("MQTT connected (provisioning)");
            s_state = PROV_CONNECTED;
        }
        break;

    case MQTT_EVT_SUBACK:
        LOG_INF("SUBACK (id=%d)", evt->param.suback.message_id);
        s_state = PROV_CONNECTED;
        break;

    case MQTT_EVT_DISCONNECT:
        LOG_INF("MQTT_EVT_DISCONNECT result=%d state=%d", evt->result, s_state);
        if (s_state == PROV_WAIT_CONNACK || s_state == PROV_WAIT_SUBACK) {
            s_state = PROV_FAILED;
        }
        break;

    case MQTT_EVT_PUBLISH: {
        const struct mqtt_publish_param *p = &evt->param.publish;
        size_t plen = p->message.payload.len;

        char topic[256] = {0};
        size_t tlen = MIN(p->message.topic.topic.size, sizeof(topic) - 1);
        memcpy(topic, p->message.topic.topic.utf8, tlen);
        LOG_INF("MQTT_EVT_PUBLISH: topic='%s' len=%u", topic, (unsigned)plen);

        if (plen >= sizeof(s_payload_buf)) {
            LOG_ERR("Payload %u > buf %u — ABORTING",
                    (unsigned)plen, (unsigned)(sizeof(s_payload_buf) - 1));
            /* Must drain to keep MQTT stream in sync */
            mqtt_readall_publish_payload(client, s_payload_buf,
                                         sizeof(s_payload_buf) - 1);
            s_state = PROV_FAILED;
            break;
        }

        rc = mqtt_readall_publish_payload(client, s_payload_buf, plen);
        if (rc) {
            LOG_ERR("mqtt_readall_publish_payload failed: %d", rc);
            s_state = PROV_FAILED;
            break;
        }
        s_payload_buf[plen] = '\0';

        if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
            struct mqtt_puback_param ack = { .message_id = p->message_id };
            mqtt_publish_qos1_ack(client, &ack);
        }

        /* ── Route by topic ─────────────────────────────────────────────── */

        if (strcmp(topic, TOPIC_CREATE_CSR_OK) == 0) {
            LOG_INF("CreateCertificateFromCsr accepted");
            cJSON *root = cJSON_Parse((char *)s_payload_buf);
            if (!root) { LOG_ERR("JSON parse failed"); s_state = PROV_FAILED; break; }

            const char *cert  = cJSON_GetStringValue(
                                    cJSON_GetObjectItem(root, "certificatePem"));
            const char *token = cJSON_GetStringValue(
                                    cJSON_GetObjectItem(root, "certificateOwnershipToken"));
            const char *cid   = cJSON_GetStringValue(
                                    cJSON_GetObjectItem(root, "certificateId"));

            if (!cert || !token) {
                LOG_ERR("missing cert or token");
                cJSON_Delete(root); s_state = PROV_FAILED; break;
            }

            strncpy(s_cert,  cert,  CERT_BUF_SIZE  - 1); s_cert[CERT_BUF_SIZE  - 1] = '\0';
            strncpy(s_token, token, TOKEN_BUF_SIZE - 1); s_token[TOKEN_BUF_SIZE - 1] = '\0';
            if (cid) { strncpy(s_cert_id, cid, sizeof(s_cert_id) - 1); }
            cJSON_Delete(root);

            LOG_INF("Certificate received (id=%.20s...)", s_cert_id);
            s_state = PROV_GOT_CERT;

        } else if (strcmp(topic, TOPIC_CREATE_CSR_FAIL) == 0) {
            LOG_ERR("CreateCertificateFromCsr REJECTED: %.100s", s_payload_buf);
            s_state = PROV_FAILED;

        } else if (strcmp(topic, topic_register_ok) == 0) {
            LOG_INF("RegisterThing accepted");
            cJSON *root = cJSON_Parse((char *)s_payload_buf);
            if (root) {
                const char *name = cJSON_GetStringValue(
                                       cJSON_GetObjectItem(root, "thingName"));
                if (name) { LOG_INF("AWS Thing: '%s'", name); }
                cJSON_Delete(root);
            }
            /*
             * Write the certificate to modem tag 21.
             * The private key is already at tag 21 (written by AT%KEYGEN).
             * modem_key_mgmt_write requires the modem to be offline.
             * Disconnect MQTT first, then take modem offline to write,
             * then restore normal (online) mode.
             *
             * Wait 500 ms after OFFLINE transition before writing — the modem
             * needs time to fully deactivate before credential operations are
             * accepted. Without this, modem_key_mgmt_write can return -EPERM
             * even though the mode was set correctly.
             */
            mqtt_disconnect(client, NULL);
            client->transport.tls.sock = -1;

            LOG_INF("Taking modem offline for cert write...");
            lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE);
            k_sleep(K_MSEC(500));  /* wait for modem to fully deactivate */

            LOG_INF("Writing device cert to modem (tag %d, len=%zu)...",
                    CONFIG_STRATUS_DEVICE_CERT_TAG, strlen(s_cert));
            rc = modem_key_mgmt_write(CONFIG_STRATUS_DEVICE_CERT_TAG,
                                       MODEM_KEY_MGMT_CRED_TYPE_PUBLIC_CERT,
                                       s_cert, strlen(s_cert));

            if (rc) {
                LOG_ERR("Failed to write device cert to modem: %d", rc);
                /* Restore modem to normal mode even on failure so caller
                 * can clean up properly */
                lte_lc_func_mode_set(LTE_LC_FUNC_MODE_NORMAL);
                s_state = PROV_FAILED;
            } else {
                LOG_INF("Device cert written to modem (tag %d)",
                        CONFIG_STRATUS_DEVICE_CERT_TAG);

                /* Verify the write actually persisted before declaring done.
                 * modem_key_mgmt_exists works in OFFLINE mode. */
                bool cert_exists = false;
                modem_key_mgmt_exists(CONFIG_STRATUS_DEVICE_CERT_TAG,
                                      MODEM_KEY_MGMT_CRED_TYPE_PUBLIC_CERT,
                                      &cert_exists);
                LOG_INF("Cert verify after write: exists=%d", (int)cert_exists);

                /* Leave modem in OFFLINE mode — main.c will call
                 * lte_lc_power_off() for a clean NVM-flushing shutdown
                 * before rebooting. Do NOT call NORMAL here; that would
                 * re-activate LTE only to immediately shut it down. */

                if (!cert_exists) {
                    LOG_ERR("Cert write reported success but cert not found!");
                    lte_lc_func_mode_set(LTE_LC_FUNC_MODE_NORMAL);
                    s_state = PROV_FAILED;
                } else {
                    s_state = PROV_DONE;
                }
            }

        } else if (strcmp(topic, topic_register_fail) == 0) {
            LOG_ERR("RegisterThing REJECTED: %.*s",
                    (int)MIN(plen, (size_t)300), s_payload_buf);
            s_state = PROV_FAILED;

        } else {
            LOG_DBG("Unhandled topic: %s", topic);
        }
        break;
    }

    case MQTT_EVT_PUBACK:
        LOG_DBG("PUBACK id=%d", evt->param.puback.message_id);
        break;

    case MQTT_EVT_PINGRESP:
        break;

    default:
        LOG_DBG("Unhandled MQTT evt type=%d", evt->type);
        break;
    }
}

/* ── run_provisioning ────────────────────────────────────────────────────── */

int provision_prepare_csr(void)
{
    return generate_csr(CONFIG_STRATUS_DEVICE_CERT_TAG,
                        s_csr, sizeof(s_csr));
}

int run_provisioning(const char *device_id, struct mqtt_client *client)
{
    int ret;

    snprintf(topic_register, sizeof(topic_register),
             "$aws/provisioning-templates/%s/provision/json",
             CONFIG_STRATUS_FLEET_TEMPLATE);
    snprintf(topic_register_ok, sizeof(topic_register_ok),
             "$aws/provisioning-templates/%s/provision/json/accepted",
             CONFIG_STRATUS_FLEET_TEMPLATE);
    snprintf(topic_register_fail, sizeof(topic_register_fail),
             "$aws/provisioning-templates/%s/provision/json/rejected",
             CONFIG_STRATUS_FLEET_TEMPLATE);

    g_device_id = device_id;
    s_state     = PROV_IDLE;

    LOG_INF("run_provisioning: device=%s  template=%s",
            device_id, CONFIG_STRATUS_FLEET_TEMPLATE);

    if (strlen(s_csr) == 0) {
        LOG_ERR("CSR not prepared — provision_prepare_csr() must succeed first");
        return -EINVAL;
    }

    /* ── Step 2: CreateCertificateFromCsr ─────────────────────────────── */
    LOG_INF("=== Phase 1: CreateCertificateFromCsr ===");
    LOG_INF("P1: calling mqtt_connect, broker=%p client=%p",
            (void *)client->broker, (void *)client);
    LOG_INF("P1: client.internal.state=%u mutex_val=%u",
            (unsigned)client->internal.state,
            (unsigned)client->internal.mutex.kernel_mutex.lock_count);

    s_state = PROV_WAIT_CONNACK;
    ret = mqtt_connect(client);
    if (ret) { LOG_ERR("Phase 1: mqtt_connect failed: %d", ret); return ret; }

    ret = drive_until(client, PROV_CONNECTED, 15000);
    if (ret) {
        LOG_ERR("Phase 1: CONNACK failed: %d", ret);
        mqtt_disconnect(client, NULL);
        return ret;
    }

    s_state = PROV_WAIT_SUBACK;
    ret = subscribe_create_csr(client);
    if (ret) { mqtt_disconnect(client, NULL); return ret; }

    ret = drive_until(client, PROV_CONNECTED, 10000);
    if (ret) {
        LOG_ERR("Phase 1: SUBACK failed: %d", ret);
        mqtt_disconnect(client, NULL);
        return ret;
    }

    /*
     * Build CreateCertificateFromCsr request:
     * { "certificateSigningRequest": "<CSR PEM>" }
     */
    static char pub_buf[PUB_BUF_SIZE] __aligned(4);
    cJSON *req = cJSON_CreateObject();
    if (!req) { mqtt_disconnect(client, NULL); return -ENOMEM; }
    cJSON_AddStringToObject(req, "certificateSigningRequest", s_csr);
    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!req_str) { mqtt_disconnect(client, NULL); return -ENOMEM; }

    if (strlen(req_str) >= sizeof(pub_buf)) {
        LOG_ERR("CSR request too large: %zu bytes", strlen(req_str));
        cJSON_free(req_str);
        mqtt_disconnect(client, NULL);
        return -ENOMEM;
    }
    strncpy(pub_buf, req_str, sizeof(pub_buf) - 1);
    cJSON_free(req_str);

    LOG_INF("Phase 1: publishing CreateCertificateFromCsr (%zu bytes)...",
            strlen(pub_buf));
    ret = pub_str(client, TOPIC_CREATE_CSR, pub_buf);
    if (ret) {
        LOG_ERR("Phase 1: publish failed: %d", ret);
        mqtt_disconnect(client, NULL);
        return ret;
    }

    /* Wait for AWS response — should be ~1.7 KB, within 2 KB modem limit */
    ret = drive_until(client, PROV_GOT_CERT, 30000);
    if (ret || s_state != PROV_GOT_CERT) {
        LOG_ERR("Phase 1: did not receive certificate (state=%d ret=%d)",
                s_state, ret);
        mqtt_disconnect(client, NULL);
        return ret ? ret : -ENODATA;
    }

    LOG_INF("Phase 1: certificate and ownership token received");
    mqtt_disconnect(client, NULL);

    /*
     * Reset the TLS socket fd to -1 before the Phase 2 reconnect.
     *
     * mqtt_disconnect() closes the socket but the MQTT library does NOT
     * reset transport.tls.sock back to -1 on NCS v3.2.1 — the stale fd
     * value remains in the struct.  When Phase 2 calls mqtt_connect()
     * on the same client struct, the library checks tls.sock:
     *   • fd >= 0 → library thinks a socket is already open and skips
     *               opening a new one → TLS handshake never happens →
     *               CONNACK times out.
     *   • fd == -1 → library opens a fresh socket → TLS handshake runs.
     *
     * Setting it to -1 here is the documented safe state for a disconnected
     * TLS client (see mqtt_client_setup() for the same pattern on init).
     */
    client->transport.tls.sock = -1;

    k_sleep(K_MSEC(500));

    /* ── Step 3: RegisterThing ─────────────────────────────────────────── */
    LOG_INF("=== Phase 2: RegisterThing (device=%s) ===", device_id);

    s_state = PROV_WAIT_CONNACK;
    ret = mqtt_connect(client);
    if (ret) { LOG_ERR("Phase 2: mqtt_connect failed: %d", ret); return ret; }

    ret = drive_until(client, PROV_CONNECTED, 15000);
    if (ret) {
        LOG_ERR("Phase 2: CONNACK failed: %d", ret);
        mqtt_disconnect(client, NULL);
        return ret;
    }

    s_state = PROV_WAIT_SUBACK;
    ret = subscribe_register(client);
    if (ret) { mqtt_disconnect(client, NULL); return ret; }

    ret = drive_until(client, PROV_CONNECTED, 10000);
    if (ret) {
        LOG_ERR("Phase 2: SUBACK failed: %d", ret);
        mqtt_disconnect(client, NULL);
        return ret;
    }

    cJSON *reg_req    = cJSON_CreateObject();
    cJSON *reg_params = cJSON_CreateObject();
    if (!reg_req || !reg_params) {
        cJSON_Delete(reg_req); cJSON_Delete(reg_params);
        mqtt_disconnect(client, NULL);
        return -ENOMEM;
    }
    cJSON_AddStringToObject(reg_req, "certificateOwnershipToken", s_token);
    cJSON_AddItemToObject(reg_req, "parameters", reg_params);
    cJSON_AddStringToObject(reg_params, "SerialNumber", device_id);
    char *reg_str = cJSON_PrintUnformatted(reg_req);
    cJSON_Delete(reg_req);
    if (!reg_str) { mqtt_disconnect(client, NULL); return -ENOMEM; }

    LOG_INF("Phase 2: publishing RegisterThing...");
    ret = pub_str(client, topic_register, reg_str);
    cJSON_free(reg_str);
    if (ret) {
        LOG_ERR("Phase 2: publish failed: %d", ret);
        mqtt_disconnect(client, NULL);
        return ret;
    }

    ret = drive_until(client, PROV_DONE, 30000);
    if (ret || s_state != PROV_DONE) {
        LOG_ERR("Phase 2: RegisterThing failed (state=%d ret=%d)",
                s_state, ret);
        mqtt_disconnect(client, NULL);
        return ret ? ret : -ENODATA;
    }

    mqtt_disconnect(client, NULL);
    LOG_INF("run_provisioning: Fleet Provisioning completed successfully");
    return 0;
}
