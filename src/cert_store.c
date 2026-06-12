/*
 * cert_store.c — TLS credential management (Phase 2)
 *
 * Responsibilities
 * ─────────────────
 * 1. Download the AWS Root CA PEM from the URL provided by config_fetch().
 *    This certificate is used by the modem to verify the AWS IoT Core MQTT
 *    broker and the API Gateway HTTPS endpoint.
 *
 * 2. Write the downloaded Root CA into modem secure NVM under the tag
 *    defined by CONFIG_CONEXIO_CLOUD_CA_TAG (default 100).
 *
 * 3. Verify that a device certificate and private key are already present
 *    in the modem (placed there by fleet provisioning).  If they are missing,
 *    return an error with instructions — cert_store does NOT generate or
 *    provision device credentials itself.
 *
 * Certificate layout in modem NVM (security tag numbers):
 *   Tag 98   — Bootstrap CA for the Root CA download server (amazontrust.com)
 *   Tag 99   — Conexio config service Root CA (config.conexio.io)
 *   Tag 100  — AWS Root CA (downloaded, used for MQTT + HTTPS)
 *   Tag 101  — Device public certificate (from fleet provisioning)
 *   Tag 102  — Device private key        (from fleet provisioning)
 *
 * Design note — write-once:
 *   cert_store_provision_from_config() checks whether each certificate
 *   already exists before writing.  This protects modem NVM write cycles
 *   (~10,000 per tag) and saves ~300 ms startup time on warm boots.
 */

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>           /* POSIX BSD sockets              */
#include <zephyr/net/tls_credentials.h>  /* TLS_SEC_TAG_LIST etc.          */
#include <zephyr/net/http/client.h>       /* Lightweight HTTP/1.1 client    */
#include <modem/modem_key_mgmt.h>         /* modem_key_mgmt_write/exists    */
#include <zephyr/logging/log.h>
#include <string.h>
#include "cert_store.h"
#include "config_fetch.h"

LOG_MODULE_REGISTER(cert_store, LOG_LEVEL_INF);

/* ── Bootstrap CA for the Root CA download ────────────────────────────────
 *
 * To download the AWS Root CA over HTTPS we need to trust the server that
 * hosts it (www.amazontrust.com).  This bootstrap CA is the trust anchor
 * for that download host — it is public, static, and never changes.
 *
 * It is NOT the certificate used for MQTT or API calls; it is only used for
 * the one-time download of the actual AWS Root CA.
 *
 * Replace with ISRG Root X1 (Let's Encrypt) or Amazon Trust Services CA
 * depending on which CA signed the certificate of your download host.
 */
static const char download_bootstrap_ca[] =
    "-----BEGIN CERTIFICATE-----\n"
    /*
     * Paste the PEM for the CA that signs amazontrust.com (or your CA
     * download host).  This is a public CA certificate — safe to embed.
     * See: https://www.amazontrust.com/repository/
     */
    "REPLACE_WITH_ISRG_ROOT_X1_OR_AMAZON_TRUST_SERVICES_CA\n"
    "-----END CERTIFICATE-----\n";

/* Modem security tag for the bootstrap CA (used only during download) */
#define DOWNLOAD_CA_TAG 98

/* Receive and body buffers for the HTTP download.
 * AWS Root CA PEM files are ~1.2 KB; 4 KB is more than sufficient. */
static uint8_t ca_recv_buf[4096];
static char    ca_body[4096];     /* Downloaded PEM content                */
static int     ca_body_len = 0;   /* Actual byte count of the PEM          */

/* ── HTTP response callback for Root CA download ──────────────────────────
 *
 * Called by http_client_req() when the response is complete.
 * We copy the body (the PEM certificate) into ca_body so the caller
 * can pass it directly to modem_key_mgmt_write().
 */
static void ca_http_response(struct http_response *rsp,
                              enum http_final_call final_data,
                              void *user_data)
{
    ARG_UNUSED(user_data);

    if (final_data == HTTP_DATA_FINAL && rsp->http_status_code == 200) {
        /* Cap copy at buffer size - 1 to leave room for null terminator */
        size_t copy_len = MIN(rsp->body_frag_len, sizeof(ca_body) - 1);
        memcpy(ca_body, rsp->body_frag_start, copy_len);
        ca_body[copy_len] = '\0';
        ca_body_len = (int)copy_len;
    }
    /* Non-200 responses leave ca_body_len = 0; the caller handles the error */
}

/* ── download_root_ca — fetch PEM from URL into ca_body ──────────────────
 *
 * Opens a TLS connection to the download host and performs an HTTP GET.
 * The URL format expected is: https://<host>/<path>
 *
 * Steps:
 *   1. Parse the hostname and path from the URL string.
 *   2. Provision the bootstrap CA into the modem (write-once).
 *   3. Open a TLS socket using the bootstrap CA for verification.
 *   4. DNS-resolve and connect to the host on port 443.
 *   5. Send GET <path> and wait for the response via ca_http_response().
 *
 * On success ca_body contains the PEM text and ca_body_len > 0.
 *
 * @param url   Full HTTPS URL to the Root CA PEM file.
 * @return 0 on success, -EINVAL for a malformed URL, negative errno otherwise.
 */
static int download_root_ca(const char *url)
{
    /* ── URL parsing ────────────────────────────────────────────────────
     * Expected format: https://<host>[/<path>]
     * Find the host by skipping "https://" and stopping at the first '/'.
     */
    const char *host_start = strstr(url, "://");
    if (!host_start) {
        LOG_ERR("Malformed Root CA URL (missing ://): %s", url);
        return -EINVAL;
    }
    host_start += 3; /* skip past "://" */

    char host[128] = {0};
    const char *path_start = strchr(host_start, '/');
    if (!path_start) {
        LOG_ERR("Malformed Root CA URL (no path): %s", url);
        return -EINVAL;
    }

    /* Copy just the host portion (up to but not including the '/') */
    size_t host_len = MIN((size_t)(path_start - host_start), sizeof(host) - 1);
    memcpy(host, host_start, host_len);

    /* ── Provision bootstrap CA (write-once) ────────────────────────────
     * Check before writing to protect modem NVM write cycles.
     */
    bool exists;
    int ret = modem_key_mgmt_exists(DOWNLOAD_CA_TAG,
                                    MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, &exists);
    if (ret != 0 || !exists) {
        LOG_INF("Provisioning download bootstrap CA (tag %d)", DOWNLOAD_CA_TAG);
        modem_key_mgmt_write(DOWNLOAD_CA_TAG,
                             MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
                             download_bootstrap_ca,
                             strlen(download_bootstrap_ca));
    }

    /* ── Open TLS socket ────────────────────────────────────────────────
     * Use DOWNLOAD_CA_TAG so the modem verifies the download server cert.
     * TLS_HOSTNAME sets the SNI extension (server name indication).
     */
    int sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
    if (sock < 0) {
        LOG_ERR("Socket creation failed (%d)", errno);
        return -errno;
    }

    sec_tag_t tags[] = { DOWNLOAD_CA_TAG };
    zsock_setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST, tags, sizeof(tags));
    zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME, host, strlen(host));

    /* ── DNS + connect ──────────────────────────────────────────────────*/
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res;
    ret = getaddrinfo(host, "443", &hints, &res);
    if (ret) {
        LOG_ERR("DNS resolution failed for %s", host);
        zsock_close(sock);
        return -ENOENT;
    }

    ret = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (ret) {
        LOG_ERR("TCP connect to %s:443 failed (%d)", host, errno);
        zsock_close(sock);
        return -errno;
    }

    /* ── HTTP GET ───────────────────────────────────────────────────────
     * path_start points to the '/' at the start of the path portion.
     * 15 s is ample time for a ~1.2 KB certificate download.
     */
    ca_body_len = 0;
    struct http_request req = {
        .method       = HTTP_GET,
        .url          = path_start,
        .host         = host,
        .protocol     = "HTTP/1.1",
        .response     = ca_http_response,
        .recv_buf     = ca_recv_buf,
        .recv_buf_len = sizeof(ca_recv_buf),
    };

    LOG_INF("Downloading Root CA from %s%s", host, path_start);
    ret = http_client_req(sock, &req, K_SECONDS(15), NULL);
    zsock_close(sock);

    if (ret < 0 || ca_body_len == 0) {
        LOG_ERR("Root CA download failed (ret=%d body=%d)", ret, ca_body_len);
        return ret < 0 ? ret : -EIO;
    }

    LOG_INF("Root CA downloaded (%d bytes)", ca_body_len);
    return 0;
}

/* ── cert_store_provision_from_config — public entry point ───────────────
 *
 * Called once by conexio_cloud_init() after config_fetch() succeeds.
 *
 * Actions:
 *   1. Check if the AWS Root CA is already in modem NVM.
 *      - If not: download it from cfg->root_ca_url and write it to modem.
 *      - If yes: skip (write-once semantics).
 *
 *   2. Check if the device certificate and private key exist in modem NVM.
 *      - These must have been placed there by fleet provisioning before
 *        this function is called.
 *      - If missing: log an actionable error and return -ENOENT so
 *        conexio_cloud_init() can report the failure to the application.
 *
 * @param cfg  Config struct returned by config_fetch(); provides root_ca_url.
 * @return     0 on success, -ENOENT if device cert/key are missing,
 *             negative errno on download or write failure.
 */
int cert_store_provision_from_config(const struct conexio_cloud_config_t *cfg)
{
    /* ── Step 1: AWS Root CA ────────────────────────────────────────────
     * Tags 100 = CA, 101 = device cert, 102 = device key (Kconfig defaults).
     * We only check and potentially write the CA here.
     */
    bool ca_exists;
    int ret = modem_key_mgmt_exists(CONFIG_CONEXIO_CLOUD_CA_TAG,
                                    MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
                                    &ca_exists);

    if (ret != 0 || !ca_exists) {
        /* CA not present — download it from the URL in the fetched config */
        LOG_INF("Root CA not found in modem — downloading from %s",
                cfg->root_ca_url);

        ret = download_root_ca(cfg->root_ca_url);
        if (ret) {
            LOG_ERR("Failed to download Root CA (%d)", ret);
            return ret;
        }

        /* Write the downloaded PEM into modem NVM */
        ret = modem_key_mgmt_write(CONFIG_CONEXIO_CLOUD_CA_TAG,
                                   MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
                                   ca_body,
                                   ca_body_len);
        if (ret) {
            LOG_ERR("modem_key_mgmt_write (Root CA) failed (%d)", ret);
            return ret;
        }
        LOG_INF("Root CA stored in modem (tag %d, %d bytes)",
                CONFIG_CONEXIO_CLOUD_CA_TAG, ca_body_len);

    } else {
        /* CA already present — skip the download to save time and NVM cycles */
        LOG_DBG("Root CA already in modem (tag %d) — skipping download",
                CONFIG_CONEXIO_CLOUD_CA_TAG);
    }

    /* ── Step 2: Device certificate and key ─────────────────────────────
     *
     * These credentials are written by the fleet provisioning process
     * (firmware/fleet-provisioning/).  Phase 2 does not create or manage
     * them — it only verifies they are present before trying to connect.
     *
     * If missing, the user needs to run fleet provisioning first.
     */
    bool cert_exists = false;
    bool key_exists  = false;

    modem_key_mgmt_exists(CONFIG_CONEXIO_CLOUD_CERT_TAG,
                          MODEM_KEY_MGMT_CRED_TYPE_PUBLIC_CERT, &cert_exists);
    modem_key_mgmt_exists(CONFIG_CONEXIO_CLOUD_KEY_TAG,
                          MODEM_KEY_MGMT_CRED_TYPE_PRIVATE_CERT, &key_exists);

    if (!cert_exists || !key_exists) {
        LOG_ERR("Device certificate (tag %d, present=%d) or private key "
                "(tag %d, present=%d) not found in modem.",
                CONFIG_CONEXIO_CLOUD_CERT_TAG, (int)cert_exists,
                CONFIG_CONEXIO_CLOUD_KEY_TAG,  (int)key_exists);
        LOG_ERR("Flash the fleet-provisioning firmware first to provision "
                "a unique device identity.");
        return -ENOENT;
    }

    LOG_INF("All TLS credentials present and ready");
    return 0;
}
