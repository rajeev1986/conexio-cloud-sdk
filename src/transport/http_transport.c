/*
 * http_transport.c — HTTPS POST transport backend (Phase 2)
 *
 * Implements the internal transport interface (transport.h) for HTTP.
 *
 * Phase 2 difference from Phase 1
 * ─────────────────────────────────
 * In Phase 1 the API Gateway hostname and API key are read from Kconfig.
 * In Phase 2 both come from config_fetch() at runtime — nothing is
 * hard-coded in prj.conf.
 *
 * HTTP vs MQTT — tradeoffs
 * ─────────────────────────
 *   HTTP advantages:
 *     - Simpler setup: only an API key, no device certificate required
 *       for the ingest endpoint (auth is at the API Gateway level).
 *     - Works well for deeply-sleeping sensors that wake infrequently.
 *
 *   HTTP limitations:
 *     - One-way telemetry only: the device cannot receive real-time
 *       command pushes from the cloud.
 *     - Commands are delivered by polling GET /v1/device-commands after
 *       each ingest call (see poll_and_execute_commands() below).
 *     - OTA Config is delivered by polling GET /v1/devices/<id>/config/pending
 *       after each ingest call (see poll_pending_config() below).
 *
 * Wake cycle (HTTP device):
 *   1. Wake from PSM / deep sleep.
 *   2. Reconnect to LTE.
 *   3. POST /v1/ingest           — send telemetry.
 *   4. GET  /v1/device-commands  — receive and execute queued commands.
 *   5. GET  /v1/devices/<id>/config/pending  — apply any pending OTA config.
 *   6. Return to deep sleep.
 *
 * Auth: same x-api-key for all three endpoints.
 */

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>           /* BSD socket API                  */
#include <zephyr/net/tls_credentials.h>  /* TLS security tag management     */
#include <zephyr/net/http/client.h>       /* Lightweight HTTP/1.1 client     */
#include <zephyr/logging/log.h>
#include <modem/lte_lc.h>                 /* lte_lc_func_mode_set for decommission */
#include <string.h>
#include <cJSON.h>                        /* JSON parsing for command queue  */
#include <cJSON_os.h>
#include "../transport.h"
#include "../config_fetch.h"

LOG_MODULE_REGISTER(http_transport, LOG_LEVEL_DBG);

/* API Gateway always listens on port 443 (HTTPS) */
#define API_PORT   443
#define API_PATH   "/v1/ingest"        /* Telemetry ingest endpoint         */

/* Modem security tag for the AWS Root CA used to verify API Gateway TLS.
 * This tag is shared with the MQTT transport. */
#define CA_TAG  CONFIG_CONEXIO_CLOUD_HTTP_CA_TAG

/* ── Runtime state populated by transport_init_with_config() ─────────────*/

static char g_api_host[128];         /* API Gateway hostname (no https://) */
static char g_api_key[128];          /* Ingest API key                     */
static char g_device_id[32];         /* IMEI-derived device ID             */

/*
 * Pre-built header string for the x-api-key header.
 * Built once at init so transport_publish() can use it directly.
 * Format: "x-api-key: <key>\r\n"
 */
static char g_api_key_header[160];

/*
 * HTTP is connectionless — each request opens and closes its own TCP socket.
 * "connected" just means init has been done and we're ready to send.
 * transport_connect() sets it to true; transport_disconnect() clears it.
 */
static bool connected = false;

/* Shared receive buffer for all HTTP requests (responses are small) */
static uint8_t recv_buf[1024];

/* Response body buffer shared by command poll and config poll */
static char    response_body[2048];
static int     response_body_len = 0;
static int     last_http_status  = 0; /* Status code of the last response  */

/* ── HTTP response callbacks ──────────────────────────────────────────────
 *
 * Each of the three request types (ingest, command poll, config poll) has
 * its own callback so we can handle the body differently.  All three share
 * the same receive buffer since requests are issued sequentially.
 */

/*
 * ingest_response_cb — callback for POST /v1/ingest.
 *
 * The ingest endpoint returns 202 Accepted on success.
 * 403 means the device has been decommissioned — stop transmitting.
 */
static void ingest_response_cb(struct http_response *rsp,
                                enum http_final_call final_data,
                                void *user_data)
{
    ARG_UNUSED(user_data);
    if (final_data == HTTP_DATA_FINAL) {
        last_http_status = rsp->http_status_code;
        if (rsp->http_status_code == 403) {
            /* Device has been decommissioned on the Conexio Console.
             * Continuing to ingest data is pointless and wastes battery.
             * Power off the modem and sleep indefinitely.
             * A full re-provisioning via the dashboard is required to recover. */
            LOG_ERR("HTTP 403 — device decommissioned. Powering down radio.");
            LOG_ERR("Re-provision this device on the Conexio Console to recover.");
            k_sleep(K_SECONDS(2)); /* flush logs */
            lte_lc_func_mode_set(LTE_LC_FUNC_MODE_POWER_OFF);
            while (1) { k_sleep(K_HOURS(24)); }
        } else if (rsp->http_status_code != 200 &&
                   rsp->http_status_code != 201 &&
                   rsp->http_status_code != 202) {
            LOG_WRN("Ingest returned HTTP %d (expected 202)",
                    rsp->http_status_code);
        }
    }
}

/*
 * poll_response_cb — callback for GET /v1/device-commands and
 *                    GET /v1/devices/<id>/config/pending.
 *
 * Copies the response body into response_body for the caller to parse.
 */
static void poll_response_cb(struct http_response *rsp,
                              enum http_final_call final_data,
                              void *user_data)
{
    ARG_UNUSED(user_data);
    if (final_data == HTTP_DATA_FINAL) {
        last_http_status = rsp->http_status_code;
        if (rsp->http_status_code == 200 && rsp->body_frag_len > 0) {
            size_t copy = MIN(rsp->body_frag_len, sizeof(response_body) - 1);
            memcpy(response_body, rsp->body_frag_start, copy);
            response_body[copy] = '\0';
            response_body_len = (int)copy;
        } else {
            response_body_len = 0;
        }
    }
}

/*
 * ack_response_cb — callback for PUT ACK requests.
 * We only need the status code; no body parsing required.
 */
static void ack_response_cb(struct http_response *rsp,
                             enum http_final_call final_data,
                             void *user_data)
{
    ARG_UNUSED(user_data);
    if (final_data == HTTP_DATA_FINAL) {
        last_http_status = rsp->http_status_code;
    }
}

/* ── TLS socket helper ────────────────────────────────────────────────────
 *
 * Opens and connects a TLS socket to g_api_host:443.
 * All three request types use this same helper.
 *
 * @return  Socket file descriptor >= 0 on success, negative errno on failure.
 */
static int open_tls_socket(void)
{
    /* IPPROTO_TLS_1_2 = TLS offloaded to the modem hardware */
    int sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
    if (sock < 0) {
        LOG_ERR("Socket creation failed (errno %d)", errno);
        return -errno;
    }

    /* Tell the modem which CA to use and what hostname to expect (SNI) */
    sec_tag_t tags[] = { CA_TAG };
    zsock_setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST, tags, sizeof(tags));
    zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME,
                     g_api_host, strlen(g_api_host));

    /* DNS resolution */
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", API_PORT);

    int ret = getaddrinfo(g_api_host, port_str, &hints, &res);
    if (ret) {
        LOG_ERR("DNS resolution failed for %s", g_api_host);
        zsock_close(sock);
        return -ENOENT;
    }

    ret = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (ret) {
        LOG_ERR("TCP connect to %s:%d failed (errno %d)",
                g_api_host, API_PORT, errno);
        zsock_close(sock);
        return -errno;
    }

    return sock; /* Caller must zsock_close(sock) when done */
}

/* ── Command queue polling ────────────────────────────────────────────────
 *
 * Called after each successful ingest.  Retrieves and executes any commands
 * that were queued while the device was offline or sleeping.
 *
 * Endpoint: GET /v1/device-commands?deviceId=<id>
 * Auth:     x-api-key header (same key as ingest)
 *
 * The cloud marks commands as 'delivered' when this GET is received.
 * For each command we:
 *   1. Deliver it to the SDK via transport_on_message() (fires app handlers).
 *   2. ACK via PUT /v1/device-commands/{commandId}/ack so the dashboard
 *      shows 'acknowledged' instead of 'delivered'.
 */
static void poll_and_execute_commands(void)
{
    /* Build the query URL */
    char url[128];
    snprintf(url, sizeof(url),
             "/v1/device-commands?deviceId=%s", g_device_id);

    int sock = open_tls_socket();
    if (sock < 0) {
        LOG_WRN("Command poll: socket failed (%d) — skipping", sock);
        return;
    }

    const char *headers[] = { g_api_key_header, NULL };

    response_body_len = 0;
    struct http_request req = {
        .method        = HTTP_GET,
        .url           = url,
        .host          = g_api_host,
        .protocol      = "HTTP/1.1",
        .header_fields = headers,
        .response      = poll_response_cb,
        .recv_buf      = recv_buf,
        .recv_buf_len  = sizeof(recv_buf),
    };

    int ret = http_client_req(sock, &req, K_SECONDS(10), NULL);
    zsock_close(sock);

    if (ret < 0 || response_body_len == 0) {
        LOG_DBG("No commands pending (ret=%d body=%d)", ret, response_body_len);
        return;
    }

    /* Parse response: { "commands": [ { commandId, sk, command, payload } ] } */
    cJSON *root = cJSON_Parse(response_body);
    if (!root) return;

    const cJSON *commands = cJSON_GetObjectItem(root, "commands");
    if (!cJSON_IsArray(commands) || cJSON_GetArraySize(commands) == 0) {
        cJSON_Delete(root);
        return; /* No commands queued */
    }

    LOG_INF("HTTP command poll: %d command(s) pending",
            cJSON_GetArraySize(commands));

    /* Process commands in the order they were queued (oldest first) */
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, commands) {
        const char *command_id = cJSON_GetStringValue(
            cJSON_GetObjectItem(item, "commandId"));
        const char *sk = cJSON_GetStringValue(
            cJSON_GetObjectItem(item, "sk"));
        const char *cmd_name = cJSON_GetStringValue(
            cJSON_GetObjectItem(item, "command"));
        const cJSON *payload = cJSON_GetObjectItem(item, "payload");

        if (!command_id || !cmd_name) continue;

        /* Build the full message JSON that transport_on_message() expects.
         * This is the same format the MQTT transport receives directly. */
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "type",      "command");
        cJSON_AddStringToObject(msg, "command",   cmd_name);
        cJSON_AddStringToObject(msg, "commandId", command_id);
        if (sk)      cJSON_AddStringToObject(msg, "sk", sk);
        if (payload) cJSON_AddItemToObject(msg, "payload",
                                           cJSON_Duplicate(payload, true));

        char *msg_json = cJSON_PrintUnformatted(msg);
        cJSON_Delete(msg);

        if (msg_json) {
            LOG_INF("Executing command: %s (id=%s)", cmd_name, command_id);
            /* Dispatch to registered command handlers in the application */
            transport_on_message(msg_json, strlen(msg_json));
            cJSON_FreeString(msg_json);
        }

        /* ACK the command so the dashboard shows 'acknowledged' */
        char ack_url[128];
        snprintf(ack_url, sizeof(ack_url),
                 "/v1/device-commands/%s/ack", command_id);

        cJSON *ack_body = cJSON_CreateObject();
        cJSON_AddStringToObject(ack_body, "deviceId", g_device_id);
        if (sk) cJSON_AddStringToObject(ack_body, "sk", sk);
        cJSON_AddStringToObject(ack_body, "result", "executed");

        char *ack_json = cJSON_PrintUnformatted(ack_body);
        cJSON_Delete(ack_body);

        if (ack_json) {
            int ack_sock = open_tls_socket();
            if (ack_sock >= 0) {
                const char *ack_headers[] = {
                    "Content-Type: application/json\r\n",
                    g_api_key_header,
                    NULL,
                };
                struct http_request ack_req = {
                    .method        = HTTP_PUT,
                    .url           = ack_url,
                    .host          = g_api_host,
                    .protocol      = "HTTP/1.1",
                    .payload       = ack_json,
                    .payload_len   = strlen(ack_json),
                    .header_fields = ack_headers,
                    .response      = ack_response_cb,
                    .recv_buf      = recv_buf,
                    .recv_buf_len  = sizeof(recv_buf),
                };
                ret = http_client_req(ack_sock, &ack_req, K_SECONDS(10), NULL);
                zsock_close(ack_sock);
                if (ret < 0) {
                    LOG_WRN("Command ACK failed (%d) for id=%s", ret, command_id);
                } else {
                    LOG_DBG("Command ACK sent: id=%s", command_id);
                }
            }
            cJSON_FreeString(ack_json);
        }
    }

    cJSON_Delete(root);
}

/* ── OTA Config pull ──────────────────────────────────────────────────────
 *
 * Called after each successful ingest.  Checks whether the dashboard has
 * pushed a new config version that this device hasn't yet applied.
 *
 * Endpoint: GET /v1/devices/<id>/config/pending
 * Auth:     x-api-key header
 *
 * Response when pending:
 *   { "pending": { "configId": "...", "version": 3, "config": {...} } }
 *
 * Response when up-to-date:
 *   { "pending": null }
 *
 * For the pending config we:
 *   1. Build a synthetic config message and deliver to SDK via
 *      transport_on_message() — this fires the registered setting handlers.
 *   2. ACK via PUT /v1/devices/<id>/config/ack so the OTA Config page
 *      shows 'applied' instead of 'pending'.
 */
static void poll_pending_config(void)
{
    char url[96];
    snprintf(url, sizeof(url),
             "/v1/devices/%s/config/pending", g_device_id);

    int sock = open_tls_socket();
    if (sock < 0) {
        LOG_WRN("Config poll: socket failed (%d) — skipping", sock);
        return;
    }

    const char *headers[] = { g_api_key_header, NULL };

    response_body_len = 0;
    struct http_request req = {
        .method        = HTTP_GET,
        .url           = url,
        .host          = g_api_host,
        .protocol      = "HTTP/1.1",
        .header_fields = headers,
        .response      = poll_response_cb,
        .recv_buf      = recv_buf,
        .recv_buf_len  = sizeof(recv_buf),
    };

    int ret = http_client_req(sock, &req, K_SECONDS(10), NULL);
    zsock_close(sock);

    if (ret < 0 || response_body_len == 0) {
        return; /* No response or error */
    }

    cJSON *root = cJSON_Parse(response_body);
    if (!root) return;

    const cJSON *pending = cJSON_GetObjectItem(root, "pending");

    /* pending == null (JSON null) means device is up to date */
    if (!cJSON_IsObject(pending)) {
        LOG_DBG("OTA Config: no pending config");
        cJSON_Delete(root);
        return;
    }

    const char  *config_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(pending, "configId"));
    const cJSON *config    = cJSON_GetObjectItem(pending, "config");
    const cJSON *ver_item  = cJSON_GetObjectItem(pending, "version");
    int version = cJSON_IsNumber(ver_item) ? (int)ver_item->valuedouble : 0;

    LOG_INF("Pending OTA Config v%d (id=%s)",
            version, config_id ? config_id : "?");

    if (config && cJSON_IsObject(config)) {
        /*
         * Build a synthetic config push message matching the format the
         * MQTT transport receives.  Delivering it via transport_on_message()
         * reuses the exact same dispatch path — registered setting handlers
         * fire with typed values, no extra code needed.
         */
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "type", "config");
        cJSON_AddNumberToObject(msg, "version", (double)version);
        if (config_id) cJSON_AddStringToObject(msg, "configId", config_id);
        cJSON_AddItemToObject(msg, "config", cJSON_Duplicate(config, true));

        char *msg_json = cJSON_PrintUnformatted(msg);
        cJSON_Delete(msg);

        if (msg_json) {
            transport_on_message(msg_json, strlen(msg_json));
            cJSON_FreeString(msg_json);
        }
    }

    /* ACK via PUT /v1/devices/<id>/config/ack */
    char ack_url[96];
    snprintf(ack_url, sizeof(ack_url),
             "/v1/devices/%s/config/ack", g_device_id);

    cJSON *ack_body = cJSON_CreateObject();
    if (config_id) cJSON_AddStringToObject(ack_body, "configId", config_id);
    cJSON_AddBoolToObject(ack_body, "success", true);

    char *ack_json = cJSON_PrintUnformatted(ack_body);
    cJSON_Delete(ack_body);

    if (ack_json) {
        int ack_sock = open_tls_socket();
        if (ack_sock >= 0) {
            const char *ack_headers[] = {
                "Content-Type: application/json\r\n",
                g_api_key_header,
                NULL,
            };
            struct http_request ack_req = {
                .method        = HTTP_PUT,
                .url           = ack_url,
                .host          = g_api_host,
                .protocol      = "HTTP/1.1",
                .payload       = ack_json,
                .payload_len   = strlen(ack_json),
                .header_fields = ack_headers,
                .response      = ack_response_cb,
                .recv_buf      = recv_buf,
                .recv_buf_len  = sizeof(recv_buf),
            };
            int r = http_client_req(ack_sock, &ack_req, K_SECONDS(10), NULL);
            zsock_close(ack_sock);
            if (r < 0) {
                LOG_WRN("Config ACK failed (%d)", r);
            } else {
                LOG_INF("Config ACK sent: v%d id=%s",
                        version, config_id ? config_id : "?");
            }
        }
        cJSON_FreeString(ack_json);
    }

    cJSON_Delete(root);
}

/* ── Transport interface implementation ───────────────────────────────────*/

/*
 * transport_init_with_config — store runtime config for later use.
 *
 * Called once by conexio_cloud_init() after config_fetch() succeeds.
 * Copies the API host and key from the fetched config and pre-builds
 * the x-api-key header string so transport_publish() doesn't need to
 * reconstruct it on every call.
 */
int transport_init_with_config(const char *device_id,
                               const struct conexio_cloud_config_t *cfg)
{
    strncpy(g_api_host,   cfg->http_host,    sizeof(g_api_host)   - 1);
    strncpy(g_api_key,    cfg->http_api_key, sizeof(g_api_key)    - 1);
    strncpy(g_device_id,  device_id,         sizeof(g_device_id)  - 1);

    /* Pre-build the header: "x-api-key: <key>\r\n" */
    snprintf(g_api_key_header, sizeof(g_api_key_header),
             "x-api-key: %s\r\n", g_api_key);

    LOG_DBG("HTTP transport init: host=%s", g_api_host);
    return 0;
}

/*
 * transport_connect — mark the transport as ready.
 *
 * HTTP is connectionless (each request opens its own socket), so
 * "connect" just means we're ready to start sending.
 * Calls transport_on_connected() to notify the SDK core.
 */
int transport_connect(void)
{
    connected = true;
    transport_on_connected();
    return 0;
}

/*
 * transport_disconnect — mark transport as not ready.
 */
int transport_disconnect(void)
{
    connected = false;
    transport_on_disconnected();
    return 0;
}

/*
 * transport_publish — POST telemetry to /v1/ingest, then poll for work.
 *
 * The full HTTP wake cycle is:
 *   POST /v1/ingest                        — send telemetry (202 expected)
 *   GET  /v1/device-commands               — receive + execute queued commands
 *   GET  /v1/devices/<id>/config/pending   — apply pending OTA config
 *
 * Each request opens and closes its own TLS socket.
 *
 * @param payload  JSON telemetry string.
 * @param len      Byte length of payload.
 * @return 0 on success, negative errno on network failure.
 */
int transport_publish(const char *payload, size_t len)
{
    const char *ingest_headers[] = {
        "Content-Type: application/json\r\n",
        g_api_key_header,
        NULL,
    };

    /* Open socket for the ingest POST */
    int sock = open_tls_socket();
    if (sock < 0) {
        LOG_ERR("transport_publish: socket failed (%d)", sock);
        return sock;
    }

    last_http_status = 0;
    struct http_request req = {
        .method        = HTTP_POST,
        .url           = API_PATH,
        .host          = g_api_host,
        .protocol      = "HTTP/1.1",
        .payload       = payload,
        .payload_len   = len,
        .header_fields = ingest_headers,
        .response      = ingest_response_cb,
        .recv_buf      = recv_buf,
        .recv_buf_len  = sizeof(recv_buf),
    };

    int ret = http_client_req(sock, &req, K_SECONDS(15), NULL);
    zsock_close(sock);

    if (ret < 0) {
        LOG_ERR("POST /v1/ingest failed (%d)", ret);
        return ret;
    }

    /* Only poll for commands and config after a successful ingest.
     * 200/201/202 are all valid success codes for this endpoint. */
    if (last_http_status == 200 ||
        last_http_status == 201 ||
        last_http_status == 202) {
        LOG_DBG("Ingest OK (HTTP %d) — polling for commands and config",
                last_http_status);
        poll_and_execute_commands();
        poll_pending_config();
    }

    return 0;
}

/*
 * transport_poll — no-op for HTTP transport.
 *
 * MQTT needs polling to drive the event loop between publishes.
 * HTTP is fully request/response — there is nothing to poll.
 * We sleep for the requested duration so the calling thread yields the CPU.
 */
void transport_poll(k_timeout_t timeout)
{
    ARG_UNUSED(timeout);
    k_sleep(timeout);
}

/* transport_is_connected — true after transport_connect(), false after disconnect */
bool transport_is_connected(void)
{
    return connected;
}
