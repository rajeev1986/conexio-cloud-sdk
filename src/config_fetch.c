/*
 * config_fetch.c — Runtime configuration fetch (Phase 2)
 *
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │  Phase 2 key concept                                                    │
 * │                                                                         │
 * │  In Phase 1 the MQTT broker hostname, HTTP host, and API key are all    │
 * │  hard-coded in prj.conf.  Every device ships with the same endpoint.    │
 * │                                                                         │
 * │  In Phase 2 the firmware ships with NO endpoint configuration.  On      │
 * │  first boot the device asks "where should I connect?" by calling        │
 * │  the Conexio config service, identified only by its modem IMEI.         │
 * │  The service returns per-workspace endpoints and credentials.           │
 * │                                                                         │
 * │  This means one binary can be flashed to every device in the fleet      │
 * │  regardless of which customer workspace it belongs to.                  │
 * └─────────────────────────────────────────────────────────────────────────┘
 *
 * Wire protocol:
 *
 *   GET https://config.conexio.io/v1/device-config?imei=<15-digit-IMEI>
 *
 *   Response (JSON):
 *   {
 *     "mqttHost":   "abc.iot.us-east-1.amazonaws.com",  // MQTT broker
 *     "httpHost":   "abc.execute-api.us-east-1.amazonaws.com", // REST API
 *     "httpApiKey": "...",     // ingest API key (same key used in HTTP POST)
 *     "rootCaUrl":  "https://www.amazontrust.com/repository/AmazonRootCA1.pem",
 *     "ttlSeconds": 3600       // how long the device may cache this response
 *   }
 *
 * Security model:
 *   - The ONLY certificate ever baked into the firmware is the Root CA for
 *     config.conexio.io itself (conexio_config_ca below).
 *   - Every other certificate is dynamically fetched and stored in the modem.
 *   - The IMEI is used purely for routing — the cloud uses it to look up
 *     which workspace this device belongs to.
 *
 * Caching:
 *   - The response is cached in RAM for CONFIG_CONEXIO_CLOUD_CONFIG_CACHE_SEC
 *     (default 1 hour).  On a warm boot the cached value is returned instantly
 *     without a network round-trip.
 *   - The cache is lost on power-off; a fresh fetch happens on the next boot.
 */

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>           /* POSIX-style BSD sockets        */
#include <zephyr/net/tls_credentials.h>  /* TLS security tag management    */
#include <zephyr/net/http/client.h>       /* Lightweight HTTP/1.1 client    */
#include <modem/modem_key_mgmt.h>         /* Write certs to modem NVM       */
#include <cJSON.h>                         /* JSON parser                    */
#include <cJSON_os.h>                      /* Zephyr memory adapter for cJSON */
#include <zephyr/logging/log.h>
#include <string.h>
#include "config_fetch.h"

LOG_MODULE_REGISTER(config_fetch, LOG_LEVEL_INF);

/* ── Config service Root CA ───────────────────────────────────────────────
 *
 * This is the ONLY certificate baked into the Phase 2 firmware binary.
 * It is used exclusively to establish a TLS connection to config.conexio.io
 * so the device can download everything else.
 *
 * It is NOT a customer certificate and never needs to change across product
 * deployments.  If you self-host the config service, replace this with
 * the Root CA of your own server.
 *
 * The certificate is stored in modem secure storage under CONFIG_CA_TAG
 * (tag 99) to keep it separate from the device credentials (tags 100-102).
 */
static const char conexio_config_ca[] =
    "-----BEGIN CERTIFICATE-----\n"
    /*
     * Replace with the actual Root CA PEM for your config service endpoint.
     * For the hosted Conexio service this is provided in the developer docs.
     */
    "REPLACE_WITH_CONEXIO_CONFIG_SERVICE_ROOT_CA\n"
    "-----END CERTIFICATE-----\n";

/* Modem security tag reserved for the config service CA.
 * Must not clash with the device credential tags (100/101/102). */
#define CONFIG_CA_TAG  99

/* ── In-RAM config cache ──────────────────────────────────────────────────
 *
 * After a successful fetch the result is kept here.  Subsequent calls to
 * config_fetch() return the cached value immediately if it is still fresh
 * (age < CONFIG_CONEXIO_CLOUD_CONFIG_CACHE_SEC).
 *
 * config_valid          — true once a successful fetch has been done.
 * config_fetched_at_ms  — k_uptime_get() value at fetch time (milliseconds).
 */
static struct conexio_cloud_config_t cached_config;
static bool    config_valid          = false;
static int64_t config_fetched_at_ms  = 0;

/* Receive buffer for the HTTP response from the config service.
 * 2 KB is generous for the small JSON response. */
static uint8_t fetch_recv_buf[2048];
static char    fetch_body[2048];     /* Body extracted from response        */
static int     fetch_body_len = 0;   /* Actual bytes copied into fetch_body */

/* ── HTTP response callback ───────────────────────────────────────────────
 *
 * The Zephyr HTTP client calls this function (possibly multiple times for
 * chunked responses) with progressively more data.  HTTP_DATA_FINAL signals
 * that all data has arrived.
 *
 * We copy the body fragment into fetch_body.  The caller (config_fetch)
 * checks fetch_body_len > 0 to know a valid response arrived.
 */
static int config_http_response(struct http_response *rsp,
                                 enum http_final_call final_data,
                                 void *user_data)
{
    ARG_UNUSED(user_data);

    if (final_data == HTTP_DATA_FINAL) {
        if (rsp->http_status_code == 200 && rsp->body_frag_len > 0) {
            size_t copy_len = MIN(rsp->body_frag_len, sizeof(fetch_body) - 1);
            memcpy(fetch_body, rsp->body_frag_start, copy_len);
            fetch_body[copy_len] = '\0';
            fetch_body_len = (int)copy_len;
        } else {
            LOG_WRN("Config service returned HTTP %d", rsp->http_status_code);
            fetch_body_len = 0;
        }
    }
    return 0;
}

/* ── config_fetch — public entry point ────────────────────────────────────
 *
 * Fetches per-workspace configuration from the Conexio config service.
 *
 * On first call (or after cache expiry):
 *   1. Provision the config service CA into modem NVM (once).
 *   2. Open a TLS/HTTPS socket to config.conexio.io.
 *   3. Send GET /v1/device-config?imei=<IMEI>.
 *   4. Parse the JSON response and cache the result.
 *
 * On subsequent calls within the cache TTL:
 *   - Return immediately with the cached config (no network access).
 *
 * @param imei  15-digit IMEI string read from the modem.
 * @param out   Caller-provided struct to fill with the fetched config.
 * @return      0 on success, negative errno on failure.
 */
int config_fetch(const char *imei, struct conexio_cloud_config_t *out)
{
    /* ── Cache check ────────────────────────────────────────────────────── */
    if (config_valid) {
        int64_t age_ms = k_uptime_get() - config_fetched_at_ms;
        if (age_ms < (int64_t)CONFIG_CONEXIO_CLOUD_CONFIG_CACHE_SEC * 1000) {
            LOG_DBG("Config cache hit (age %lld s)", age_ms / 1000);
            memcpy(out, &cached_config, sizeof(*out));
            return 0;
        }
        LOG_INF("Config cache expired — re-fetching from config service");
    }

    /* ── Provision the config service CA (write-once) ───────────────────
     *
     * modem_key_mgmt_exists() checks whether the cert is already in NVM.
     * We only write it on the very first boot (or if it was somehow erased).
     * Skipping the write on warm boots saves ~300 ms and protects modem NVM
     * write cycles (limit ~10,000 per tag).
     */
    bool ca_exists;
    int ret = modem_key_mgmt_exists(CONFIG_CA_TAG,
                                    MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
                                    &ca_exists);
    if (ret != 0 || !ca_exists) {
        LOG_INF("Provisioning config service CA (tag %d)", CONFIG_CA_TAG);
        ret = modem_key_mgmt_write(CONFIG_CA_TAG,
                                   MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
                                   conexio_config_ca,
                                   strlen(conexio_config_ca));
        if (ret) {
            LOG_ERR("Config CA write to modem failed (%d)", ret);
            return ret;
        }
    }

    /* ── Build the request URL ──────────────────────────────────────────
     *
     * The full URL is:  https://<CONFIG_HOST><CONFIG_PATH>?imei=<IMEI>
     * We only pass the path+query to the HTTP client; the host is set
     * separately in the http_request struct.
     */
    char url[128];
    snprintf(url, sizeof(url), "%s?imei=%s",
             CONFIG_CONEXIO_CLOUD_CONFIG_PATH, imei);

    /* ── Open TLS socket to config service ─────────────────────────────
     *
     * IPPROTO_TLS_1_2 ensures the modem uses TLS (offloaded to modem HW).
     * TLS_SEC_TAG_LIST tells the modem which CA cert to use for verification.
     * TLS_HOSTNAME enables SNI so the server can select the right certificate.
     */
    int sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
    if (sock < 0) return -errno;

    sec_tag_t tags[] = { CONFIG_CA_TAG };
    zsock_setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST, tags, sizeof(tags));
    zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME,
                     CONFIG_CONEXIO_CLOUD_CONFIG_HOST,
                     strlen(CONFIG_CONEXIO_CLOUD_CONFIG_HOST));

    /* DNS resolution — the modem resolves the hostname via its built-in DNS */
    struct zsock_addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct zsock_addrinfo *res;
    ret = zsock_getaddrinfo(CONFIG_CONEXIO_CLOUD_CONFIG_HOST, "443", &hints, &res);
    if (ret) {
        LOG_ERR("DNS lookup failed for %s", CONFIG_CONEXIO_CLOUD_CONFIG_HOST);
        zsock_close(sock);
        return -ENOENT;
    }

    ret = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
    zsock_freeaddrinfo(res);
    if (ret) {
        LOG_ERR("TCP connect to config service failed (%d)", errno);
        zsock_close(sock);
        return -errno;
    }

    /* ── Send HTTP GET ──────────────────────────────────────────────────
     *
     * http_client_req() is synchronous up to the given timeout.
     * config_http_response() is called with the response data.
     * 15 s is generous for a small JSON response; adjust if your
     * network latency is unusually high.
     */
    fetch_body_len = 0;
    struct http_request req = {
        .method       = HTTP_GET,
        .url          = url,
        .host         = CONFIG_CONEXIO_CLOUD_CONFIG_HOST,
        .protocol     = "HTTP/1.1",
        .response     = config_http_response,
        .recv_buf     = fetch_recv_buf,
        .recv_buf_len = sizeof(fetch_recv_buf),
    };

    LOG_INF("Fetching config: GET https://%s%s",
            CONFIG_CONEXIO_CLOUD_CONFIG_HOST, url);

    ret = http_client_req(sock, &req, 15000, NULL);
    zsock_close(sock); /* Always close the socket, even on error */

    if (ret < 0 || fetch_body_len == 0) {
        LOG_ERR("Config fetch failed (ret=%d body_len=%d)", ret, fetch_body_len);
        return ret < 0 ? ret : -EIO;
    }

    /* ── Parse JSON response ────────────────────────────────────────────
     *
     * Expected fields (all strings):
     *   mqttHost    — AWS IoT Core MQTT broker hostname
     *   httpHost    — API Gateway hostname for HTTP ingest
     *   httpApiKey  — API key for the HTTP ingestor (same as prj.conf in Phase 1)
     *   rootCaUrl   — HTTPS URL to download the AWS Root CA PEM
     *
     * Missing or empty fields are silently skipped; the SDK will fail later
     * if a required field is absent (e.g. MQTT connect will fail).
     */
    cJSON *json = cJSON_Parse(fetch_body);
    if (!json) {
        LOG_ERR("JSON parse error — response: %.64s", fetch_body);
        return -EINVAL;
    }

    /* Clear the cached struct before filling it */
    memset(&cached_config, 0, sizeof(cached_config));

    const char *mqtt_host   = cJSON_GetStringValue(cJSON_GetObjectItem(json, "mqttHost"));
    const char *http_host   = cJSON_GetStringValue(cJSON_GetObjectItem(json, "httpHost"));
    const char *http_key    = cJSON_GetStringValue(cJSON_GetObjectItem(json, "httpApiKey"));
    const char *root_ca_url = cJSON_GetStringValue(cJSON_GetObjectItem(json, "rootCaUrl"));

    /* strncpy with sizeof-1 guarantees null termination even for long values */
    if (mqtt_host)   strncpy(cached_config.mqtt_host,    mqtt_host,   sizeof(cached_config.mqtt_host)    - 1);
    if (http_host)   strncpy(cached_config.http_host,    http_host,   sizeof(cached_config.http_host)    - 1);
    if (http_key)    strncpy(cached_config.http_api_key, http_key,    sizeof(cached_config.http_api_key) - 1);
    if (root_ca_url) strncpy(cached_config.root_ca_url,  root_ca_url, sizeof(cached_config.root_ca_url)  - 1);

    cJSON_Delete(json);

    /* Mark cache as valid and record the fetch time */
    config_valid          = true;
    config_fetched_at_ms  = k_uptime_get();

    /* Copy result to caller */
    memcpy(out, &cached_config, sizeof(*out));

    LOG_INF("Config OK — mqtt=%s http=%s",
            cached_config.mqtt_host[0] ? cached_config.mqtt_host : "(none)",
            cached_config.http_host[0] ? cached_config.http_host : "(none)");
    return 0;
}
